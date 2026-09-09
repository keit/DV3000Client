#include "mainwindow.h"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QVBoxLayout>

#include "reflectorlistmodel.h"
#include "settingsdialog.h"

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    m_settings = GuiSettings::load();

    setWindowTitle("DV3000 Client");

    m_searchBox = new QLineEdit;
    m_searchBox->setPlaceholderText("Search reflectors (e.g. 123, XLX123, XRF587)...");

    m_model = new ReflectorListModel(this);
    connect(m_model, &ReflectorListModel::refreshFailed, this, [this](const QString &error) {
        m_statusLabel->setText("Reflector list: " + error + " (showing static list only)");
    });

    m_proxy = new QSortFilterProxyModel(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);

    m_reflectorList = new QListView;
    m_reflectorList->setModel(m_proxy);
    m_reflectorList->setSelectionMode(QAbstractItemView::SingleSelection);

    connect(m_searchBox, &QLineEdit::textChanged, m_proxy, &QSortFilterProxyModel::setFilterFixedString);

    m_targetModule = new QComboBox;
    for (char c = 'A'; c <= 'Z'; c++) m_targetModule->addItem(QString(QChar(c)));

    m_settingsButton = new QPushButton("Settings...");
    connect(m_settingsButton, &QPushButton::clicked, this, &MainWindow::openSettings);

    m_connectButton = new QPushButton("Connect");
    connect(m_connectButton, &QPushButton::clicked, this, &MainWindow::onConnectClicked);

    m_statusLabel = new QLabel("Disconnected. Hold SPACE to transmit once connected.");

    auto *moduleRow = new QHBoxLayout;
    moduleRow->addWidget(new QLabel("Target module:"));
    moduleRow->addWidget(m_targetModule);
    moduleRow->addStretch();
    moduleRow->addWidget(m_settingsButton);

    auto *bottomRow = new QHBoxLayout;
    bottomRow->addWidget(m_connectButton);
    bottomRow->addWidget(m_statusLabel, 1);

    auto *layout = new QVBoxLayout;
    layout->addWidget(m_searchBox);
    layout->addWidget(m_reflectorList, 1);
    layout->addLayout(moduleRow);
    layout->addLayout(bottomRow);

    auto *central = new QWidget;
    central->setLayout(layout);
    setCentralWidget(central);
    resize(480, 560);

    qApp->installEventFilter(this);

    m_model->refresh();
}

MainWindow::~MainWindow() {
    qApp->removeEventFilter(this);
}

QString MainWindow::selectedHost() const {
    QModelIndex idx = m_reflectorList->currentIndex();
    if (!idx.isValid()) return {};
    QModelIndex srcIdx = m_proxy->mapToSource(idx);
    return m_model->data(srcIdx, ReflectorListModel::HostRole).toString();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
        auto *ke = static_cast<QKeyEvent *>(event);
        // Space is PTT everywhere except while actually typing in the
        // search box, where it needs to type a literal space.
        if (ke->key() == Qt::Key_Space && !ke->isAutoRepeat() &&
            qApp->focusWidget() != m_searchBox) {
            m_pttActive.store(event->type() == QEvent::KeyPress);
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::closeEvent(QCloseEvent *event) {
    // Wait out any in-flight connect/disconnect before touching the
    // session members directly below -- otherwise the worker thread could
    // still be writing to them after this window (and its members) starts
    // being destroyed.
    if (m_worker.joinable()) m_worker.join();
    if (m_connected) stopSessionBlocking();
    QMainWindow::closeEvent(event);
}

void MainWindow::openSettings() {
    SettingsDialog dlg(m_settings, this);
    if (dlg.exec() == QDialog::Accepted) {
        m_settings = dlg.settings();
        m_settings.save();
    }
}

void MainWindow::onConnectClicked() {
    if (m_busy) return;
    if (m_connected) {
        startDisconnect();
    } else {
        startConnect();
    }
}

void MainWindow::startConnect() {
    QString host = selectedHost();
    if (host.isEmpty()) {
        QMessageBox::information(this, "No reflector selected", "Pick a reflector from the list first.");
        return;
    }
    if (m_settings.thumbdvDevice.isEmpty()) {
        QMessageBox::warning(this, "No ThumbDV device", "Set your ThumbDV device in Settings first.");
        return;
    }

    char targetModule = m_targetModule->currentText().at(0).toLatin1();
    setBusy(true, "Connecting to " + host + "...");

    if (m_worker.joinable()) m_worker.join();
    m_worker = std::thread(&MainWindow::connectWorker, this, host, targetModule, m_settings);
}

void MainWindow::connectWorker(QString host, char targetModule, GuiSettings settings) {
    QString error;
    bool ok = true;

    m_dv = std::make_unique<SerialDV::DVController>();
    if (!m_dv->open(settings.thumbdvDevice.toStdString())) {
        error = "Failed to open ThumbDV device " + settings.thumbdvDevice;
        ok = false;
    }

    if (ok && !m_capture.open(settings.audioInputDevice.toStdString(), SND_PCM_STREAM_CAPTURE)) {
        error = "Failed to open audio input device " + settings.audioInputDevice;
        ok = false;
    }

    if (ok && !m_playback.open(settings.audioOutputDevice.toStdString(), SND_PCM_STREAM_PLAYBACK)) {
        error = "Failed to open audio output device " + settings.audioOutputDevice;
        ok = false;
    }

    if (ok) {
        char myModule = settings.moduleSuffix.isEmpty() ? 'B' : settings.moduleSuffix.at(0).toLatin1();
        m_client = std::make_unique<dextra::DextraClient>(
            m_dv.get(), [this](const short *pcm) { m_rxQueue.push(pcm); }, nullptr);
        m_client->setIdentity(settings.callsign.toStdString(), myModule);

        if (!m_client->open(host.toStdString(), targetModule)) {
            error = "Failed to open network socket to " + host;
            ok = false;
        }
    }

    if (ok) {
        dextra::g_running = 1;
        if (!m_client->link()) {
            error = "Reflector did not accept the connection (NAK or timeout)";
            ok = false;
        }
    }

    if (!ok) {
        m_client.reset();
        m_capture.close();
        m_playback.close();
        if (m_dv) m_dv->close();
        m_dv.reset();
    }

    QMetaObject::invokeMethod(
        this, [this, ok, error]() { onConnectFinished(ok, error); }, Qt::QueuedConnection);
}

void MainWindow::onConnectFinished(bool ok, QString error) {
    if (!ok) {
        setBusy(false, "Disconnected.");
        QMessageBox::warning(this, "Connect failed", error);
        return;
    }

    m_pttActive.store(false);
    m_captureThread = std::thread(dextra::captureThread, m_dv.get(), &m_capture, m_client.get(),
                                   [this] { return m_pttActive.load(); });
    m_playbackThread = std::thread(dextra::playbackThread, &m_playback, &m_rxQueue);
    m_networkThread = std::thread([this] { m_client->run(); });

    setConnected(true, "Connected. Hold SPACE to transmit.");
}

void MainWindow::startDisconnect() {
    setBusy(true, "Disconnecting...");
    if (m_worker.joinable()) m_worker.join();
    m_worker = std::thread(&MainWindow::disconnectWorker, this);
}

void MainWindow::disconnectWorker() {
    stopSessionBlocking();
    QMetaObject::invokeMethod(this, [this]() { onDisconnectFinished(); }, Qt::QueuedConnection);
}

void MainWindow::onDisconnectFinished() {
    m_client.reset();
    m_dv.reset();
    setConnected(false, "Disconnected.");
}

void MainWindow::stopSessionBlocking() {
    dextra::g_running = 0;
    m_pttActive.store(false);
    if (m_captureThread.joinable()) m_captureThread.join();
    if (m_playbackThread.joinable()) m_playbackThread.join();
    if (m_networkThread.joinable()) m_networkThread.join();
    if (m_client) m_client->disconnect();
    m_capture.close();
    m_playback.close();
    if (m_dv) m_dv->close();
    dextra::g_running = 1;
}

void MainWindow::setBusy(bool busy, const QString &status) {
    m_busy = busy;
    m_connectButton->setEnabled(!busy);
    m_searchBox->setEnabled(!busy && !m_connected);
    m_reflectorList->setEnabled(!busy && !m_connected);
    m_targetModule->setEnabled(!busy && !m_connected);
    m_settingsButton->setEnabled(!busy && !m_connected);
    m_statusLabel->setText(status);
}

void MainWindow::setConnected(bool connected, const QString &status) {
    m_connected = connected;
    m_connectButton->setText(connected ? "Disconnect" : "Connect");
    setBusy(false, status);
}
