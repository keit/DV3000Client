#include "mainwindow.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QVBoxLayout>

#include "reflectorlistmodel.h"
#include "settingsdialog.h"

namespace {
// Button/label colors are set explicitly (rather than left to the system
// theme) specifically to be visible cues independent of it -- white text
// reads fine on both regardless of light/dark mode.
const char *kConnectedButtonStyle = "background-color: #4CAF50; color: white;";
const char *kErrorButtonStyle = "background-color: #f44336; color: white;";
const char *kErrorLabelStyle = "color: #f44336;";
// Distinct from the error red above -- an "on-air" orange, deliberately
// hard to miss so a forgotten toggled-on transmit is obvious at a glance.
const char *kSendingButtonStyle = "background-color: #ff9800; color: white;";
// Lighter than plain white (easier on the eyes) but distinct from the
// metallic window background so content areas still read as content.
const char *kLightGrayBackground = "#e8e8e8";
} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    m_settings = GuiSettings::load();

    setWindowTitle("DV3000 Client");

    // Brushed-metal-style background, independently chosen (not sampled
    // from any other app) -- scoped to the QMainWindow selector only, so
    // it paints just the window's own background and doesn't cascade into
    // child widgets' native styling (the connect button's status colors
    // above included).
    setStyleSheet(
        "QMainWindow {"
        "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
        "    stop:0 #eef0f2, stop:0.5 #cdd0d3, stop:1 #aeb2b6);"
        "}");

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
    m_reflectorList->setStyleSheet(QString("QListView { background-color: %1; }").arg(kLightGrayBackground));

    connect(m_searchBox, &QLineEdit::textChanged, m_proxy, &QSortFilterProxyModel::setFilterFixedString);
    connect(m_reflectorList->selectionModel(), &QItemSelectionModel::currentChanged, this,
            &MainWindow::updateConnectButtonEnabled);

    // Index 0 is a placeholder, not a real module -- without it the combo
    // box would silently start on "A" already selected, letting Connect
    // send you to a module you never actually chose.
    m_targetModule = new QComboBox;
    m_targetModule->addItem("Select module...");
    for (char c = 'A'; c <= 'Z'; c++) m_targetModule->addItem(QString(QChar(c)));
    connect(m_targetModule, &QComboBox::currentIndexChanged, this, &MainWindow::updateConnectButtonEnabled);

    menuBar()->setStyleSheet(QString("QMenuBar { background-color: %1; }").arg(kLightGrayBackground));
    auto *fileMenu = menuBar()->addMenu("&File");
    m_settingsAction = fileMenu->addAction("&Settings...", this, &MainWindow::openSettings);
    m_settingsAction->setShortcut(QKeySequence::Preferences);
    fileMenu->addSeparator();
    fileMenu->addAction("&Quit", QKeySequence::Quit, this, &QWidget::close);

    m_connectButton = new QPushButton("Connect");
    connect(m_connectButton, &QPushButton::clicked, this, &MainWindow::onConnectClicked);

    // Toggle, not press-and-hold: one click/press switches into sending
    // mode and it stays there until clicked/pressed again. setCheckable
    // makes a single click flip the checked state and keep it that way;
    // toggled() fires for that and for any programmatic setChecked() call
    // (see setConnected()), so this one handler is the only place PTT
    // state, button text, and button color need to be kept in sync.
    m_pttButton = new QPushButton("PTT to send");
    m_pttButton->setEnabled(false);
    m_pttButton->setCheckable(true);
    connect(m_pttButton, &QPushButton::toggled, this, [this](bool sending) {
        m_pttActive.store(sending);
        m_pttButton->setText(sending ? "Sending (click to stop)" : "PTT to send");
        m_pttButton->setStyleSheet(sending ? kSendingButtonStyle : "");
    });

    m_statusLabel = new QLabel("Disconnected.");

    m_rpt1Label = new QLabel("—");
    m_rpt2Label = new QLabel("—");
    m_urCallLabel = new QLabel("—");
    m_myCallLabel = new QLabel("—");
    m_myCall2Label = new QLabel("—");

    auto *headerBox = new QGroupBox("Last received header");
    auto *headerForm = new QFormLayout(headerBox);
    headerForm->addRow("MYCALL:", m_myCallLabel);
    headerForm->addRow("MYCALL2:", m_myCall2Label);
    headerForm->addRow("URCALL:", m_urCallLabel);
    headerForm->addRow("RPT1:", m_rpt1Label);
    headerForm->addRow("RPT2:", m_rpt2Label);

    auto *moduleRow = new QHBoxLayout;
    moduleRow->addWidget(new QLabel("Target module:"));
    moduleRow->addWidget(m_targetModule);
    moduleRow->addStretch();

    auto *bottomRow = new QHBoxLayout;
    bottomRow->addWidget(m_connectButton);
    bottomRow->addWidget(m_statusLabel, 1);
    bottomRow->addWidget(m_pttButton);

    auto *layout = new QVBoxLayout;
    layout->addWidget(m_searchBox);
    layout->addWidget(m_reflectorList, 1);
    layout->addLayout(moduleRow);
    layout->addLayout(bottomRow);
    layout->addWidget(headerBox);

    auto *central = new QWidget;
    central->setLayout(layout);
    setCentralWidget(central);
    resize(480, 640);

    qApp->installEventFilter(this);

    m_model->refresh();
    updateConnectButtonEnabled();
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

QString MainWindow::selectedReflectorName() const {
    QModelIndex idx = m_reflectorList->currentIndex();
    if (!idx.isValid()) return {};
    QModelIndex srcIdx = m_proxy->mapToSource(idx);
    return m_model->data(srcIdx, ReflectorListModel::NameRole).toString();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
        auto *ke = static_cast<QKeyEvent *>(event);
        // Space toggles PTT everywhere except while actually typing in the
        // search box, where it needs to type a literal space. Only the
        // press toggles -- the release is still swallowed (returning true
        // for both) so it can't leak through as e.g. activating whatever
        // widget happens to have focus. toggle() flips the button's
        // checked state and emits toggled(), which is what actually drives
        // m_pttActive and the button's text/color -- see its connection above.
        if (ke->key() == Qt::Key_Space && !ke->isAutoRepeat() &&
            qApp->focusWidget() != m_searchBox) {
            if (event->type() == QEvent::KeyPress && m_pttButton->isEnabled()) m_pttButton->toggle();
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
    if (m_targetModule->currentIndex() <= 0) {
        QMessageBox::information(this, "No module selected", "Pick a target module first.");
        return;
    }
    if (m_settings.thumbdvDevice.isEmpty()) {
        QMessageBox::warning(this, "No ThumbDV device", "Set your ThumbDV device in Settings first.");
        return;
    }

    QString reflectorName = selectedReflectorName();
    char targetModule = m_targetModule->currentText().at(0).toLatin1();
    m_connectButton->setStyleSheet("");
    m_statusLabel->setStyleSheet("");
    setBusy(true, "Connecting to " + host + "...");

    if (m_worker.joinable()) m_worker.join();
    m_worker = std::thread(&MainWindow::connectWorker, this, host, reflectorName, targetModule, m_settings);
}

void MainWindow::connectWorker(QString host, QString reflectorName, char targetModule, GuiSettings settings) {
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
        // Runs on the network thread once client->run() starts -- marshal
        // to the GUI thread rather than touching widgets directly here.
        m_client->setHeaderSink([this](const dextra::DStarHeader &header) {
            QMetaObject::invokeMethod(
                this, [this, header]() { onHeaderReceived(header); }, Qt::QueuedConnection);
        });

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
        this,
        [this, ok, error, reflectorName, targetModule]() {
            onConnectFinished(ok, error, reflectorName, targetModule);
        },
        Qt::QueuedConnection);
}

void MainWindow::onConnectFinished(bool ok, QString error, QString reflectorName, char targetModule) {
    if (!ok) {
        setBusy(false, "Connect failed: " + error);
        m_connectButton->setStyleSheet(kErrorButtonStyle);
        m_statusLabel->setStyleSheet(kErrorLabelStyle);
        QMessageBox::warning(this, "Connect failed", error);
        return;
    }

    m_rpt1Label->setText("—");
    m_rpt2Label->setText("—");
    m_urCallLabel->setText("—");
    m_myCallLabel->setText("—");
    m_myCall2Label->setText("—");

    m_pttActive.store(false);
    m_captureThread = std::thread(dextra::captureThread, m_dv.get(), &m_capture, m_client.get(),
                                   [this] { return m_pttActive.load(); });
    m_playbackThread = std::thread(dextra::playbackThread, &m_playback, &m_rxQueue);
    m_networkThread = std::thread([this] { m_client->run(); });

    setConnected(true, QString("Connected to %1, module %2")
                            .arg(reflectorName.isEmpty() ? "?" : reflectorName)
                            .arg(QChar(targetModule)));
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
    m_rpt1Label->setText("—");
    m_rpt2Label->setText("—");
    m_urCallLabel->setText("—");
    m_myCallLabel->setText("—");
    m_myCall2Label->setText("—");
    setConnected(false, "Disconnected.");
}

void MainWindow::onHeaderReceived(dextra::DStarHeader header) {
    m_rpt1Label->setText(QString::fromStdString(header.rpt1));
    m_rpt2Label->setText(QString::fromStdString(header.rpt2));
    m_urCallLabel->setText(QString::fromStdString(header.urCall));
    m_myCallLabel->setText(QString::fromStdString(header.myCall));
    m_myCall2Label->setText(QString::fromStdString(header.myCall2));
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
    m_searchBox->setEnabled(!busy && !m_connected);
    m_reflectorList->setEnabled(!busy && !m_connected);
    m_targetModule->setEnabled(!busy && !m_connected);
    m_settingsAction->setEnabled(!busy && !m_connected);
    m_statusLabel->setText(status);
    updateConnectButtonEnabled();
}

void MainWindow::setConnected(bool connected, const QString &status) {
    m_connected = connected;
    m_connectButton->setText(connected ? "Disconnect" : "Connect");
    m_connectButton->setStyleSheet(connected ? kConnectedButtonStyle : "");
    m_statusLabel->setStyleSheet("");
    m_pttButton->setEnabled(connected);
    if (!connected) m_pttButton->setChecked(false); // in case we disconnected mid-send
    setBusy(false, status);
}

void MainWindow::updateConnectButtonEnabled() {
    // Once connected, the button becomes Disconnect and should always stay
    // usable regardless of what's selected in the (by then disabled)
    // reflector list and module combo.
    bool readyToConnect = !selectedHost().isEmpty() && m_targetModule->currentIndex() > 0;
    m_connectButton->setEnabled(!m_busy && (m_connected || readyToConnect));
}
