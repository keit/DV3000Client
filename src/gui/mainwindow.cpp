#include "mainwindow.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QCompleter>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QHeaderView>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTime>
#include <QVBoxLayout>

#include "filelogging.h"
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
    // above included). The focus rules below are separate, explicit type
    // selectors (QPushButton:focus etc.), so they're similarly scoped --
    // only widgets of those exact types get the thicker border, nothing
    // else picks up stray styling from this. A plain native focus
    // rectangle is easy to miss at a glance (especially tabbing through
    // with the window not front-of-mind); a solid colored border is much
    // harder not to notice.
    setStyleSheet(
        "QMainWindow {"
        "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
        "    stop:0 #eef0f2, stop:0.5 #cdd0d3, stop:1 #aeb2b6);"
        "}"
        "QPushButton:focus, QComboBox:focus, QLineEdit:focus {"
        "  border: 3px solid #2195f3;"
        "}");

    m_model = new ReflectorListModel(this);
    connect(m_model, &ReflectorListModel::refreshFailed, this, [this](const QString &error) {
        m_statusLabel->setText("Reflector list: " + error + " (showing static list only)");
    });

    // Editable combo box backed directly by the model (no filtering proxy
    // needed -- the completer below does its own filtering for the popup,
    // independent of what the combo box's model actually holds) with a
    // QCompleter standing in for the old search box + always-visible list:
    // typing narrows a popup of matches, picking one (mouse or Enter)
    // collapses it back down to a single field. NoInsert keeps a typed
    // string that doesn't match anything from trying to add itself as a
    // new item -- the model doesn't support that anyway, but the combo
    // box shouldn't attempt it.
    m_reflectorCombo = new QComboBox;
    m_reflectorCombo->setEditable(true);
    m_reflectorCombo->setInsertPolicy(QComboBox::NoInsert);
    m_reflectorCombo->setModel(m_model);
    m_reflectorCombo->setCurrentIndex(-1);
    m_reflectorCombo->lineEdit()->setPlaceholderText("Search reflectors (e.g. 123, XLX123, XRF587)...");
    m_reflectorCombo->setStyleSheet(
        QString("QComboBox QAbstractItemView { background-color: %1; }").arg(kLightGrayBackground));

    // QComboBox resets currentIndex to row 0 on its own whenever its model
    // resets -- the setCurrentIndex(-1) above only covers the very first
    // setModel() call, not the *second* reset that happens moments later
    // when the async live reflector list finishes loading and
    // ReflectorListModel::applyLive() rebuilds the row list. Without this,
    // the box would silently end up with reflector #1 "selected" (and its
    // name filled into the line edit) as soon as that background fetch
    // completes, whether or not the user has touched anything yet.
    connect(m_model, &QAbstractItemModel::modelReset, this, [this] { m_reflectorCombo->setCurrentIndex(-1); });

    auto *completer = new QCompleter(m_model, this);
    completer->setCompletionRole(Qt::DisplayRole);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains); // matches anywhere in the name/country/comment, not just a prefix
    completer->setCompletionMode(QCompleter::PopupCompletion);
    completer->popup()->setStyleSheet(QString("background-color: %1;").arg(kLightGrayBackground));
    m_reflectorCombo->setCompleter(completer);

    // Re-check on every text change, not just when currentIndex() itself
    // changes -- see hasValidReflectorSelection(). textChanged (not
    // textEdited) deliberately: re-picking the *same* row after having
    // edited away from it restores the same index it was already sitting
    // on, so currentIndexChanged doesn't fire (the value didn't change),
    // and it's a programmatic pick rather than a keystroke, so textEdited
    // doesn't fire either -- textChanged is the one signal that reliably
    // covers both typing and any kind of pick. Safe to hang on the broader
    // signal since this only recomputes state; it never touches
    // currentIndex or the line edit's text itself, so it can't trigger
    // itself into a loop the way an earlier version of this did.
    connect(m_reflectorCombo->lineEdit(), &QLineEdit::textChanged, this, &MainWindow::updateConnectButtonEnabled);

    connect(m_reflectorCombo, &QComboBox::currentIndexChanged, this, &MainWindow::updateConnectButtonEnabled);

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

    auto *helpMenu = menuBar()->addMenu("&Help");
    helpMenu->addAction("&Log File Location...", [this] {
        QMessageBox::information(this, "Log File Location", logFilePath());
    });

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

    // Newest first, capped in addLastHeardEntry() -- reflector/module come
    // from the connection itself (m_connectedReflectorName/Module), not
    // parsed out of the header, since a session only ever links to one
    // reflector+module and everything heard during it necessarily came
    // via that same one.
    m_lastHeardTable = new QTableWidget(0, 4);
    m_lastHeardTable->setHorizontalHeaderLabels({"Time", "Callsign", "Reflector", "Module"});
    m_lastHeardTable->verticalHeader()->setVisible(false);
    m_lastHeardTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_lastHeardTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_lastHeardTable->setFocusPolicy(Qt::NoFocus); // a passive log, not a tab stop
    m_lastHeardTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_lastHeardTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_lastHeardTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_lastHeardTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_lastHeardTable->setStyleSheet(QString("QTableWidget { background-color: %1; }").arg(kLightGrayBackground));

    auto *moduleRow = new QHBoxLayout;
    moduleRow->addWidget(new QLabel("Target module:"));
    moduleRow->addWidget(m_targetModule);
    moduleRow->addStretch();

    auto *bottomRow = new QHBoxLayout;
    bottomRow->addWidget(m_connectButton);
    bottomRow->addWidget(m_statusLabel, 1);
    bottomRow->addWidget(m_pttButton);

    auto *layout = new QVBoxLayout;
    layout->addWidget(m_reflectorCombo);
    layout->addLayout(moduleRow);
    layout->addLayout(bottomRow);
    layout->addWidget(headerBox);
    layout->addStretch();

    auto *leftPanel = new QWidget;
    leftPanel->setLayout(layout);

    auto *splitter = new QSplitter;
    splitter->addWidget(leftPanel);
    splitter->addWidget(m_lastHeardTable);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);

    setCentralWidget(splitter);
    resize(760, 420);

    qApp->installEventFilter(this);

    m_model->refresh();
    updateConnectButtonEnabled();
}

MainWindow::~MainWindow() {
    qApp->removeEventFilter(this);
}

// currentIndex() alone isn't enough: QComboBox doesn't automatically
// invalidate it just because the user edited the line edit's text away
// from whatever that row says (there's no "unselect on edit" built in).
// So a pick only counts if the row it points to is still what's actually
// displayed right now.
bool MainWindow::hasValidReflectorSelection() const {
    int row = m_reflectorCombo->currentIndex();
    if (row < 0) return false;
    return m_reflectorCombo->currentText() == m_model->data(m_model->index(row, 0), Qt::DisplayRole).toString();
}

QString MainWindow::selectedHost() const {
    if (!hasValidReflectorSelection()) return {};
    return m_model->data(m_model->index(m_reflectorCombo->currentIndex(), 0), ReflectorListModel::HostRole).toString();
}

QString MainWindow::selectedReflectorName() const {
    if (!hasValidReflectorSelection()) return {};
    return m_model->data(m_model->index(m_reflectorCombo->currentIndex(), 0), ReflectorListModel::NameRole).toString();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
        auto *ke = static_cast<QKeyEvent *>(event);
        // Space toggles PTT everywhere except: while actually typing in the
        // search box, where it needs to type a literal space; and while
        // the Connect/Disconnect button has focus, where it should do what
        // a focused QPushButton normally does on Space -- activate it --
        // rather than have that get swallowed for PTT before it ever gets
        // there. Only the press toggles -- the release is still swallowed
        // (returning true for both) so it can't leak through as e.g.
        // activating whatever widget happens to have focus. toggle() flips
        // the button's checked state and emits toggled(), which is what
        // actually drives m_pttActive and the button's text/color -- see
        // its connection above.
        if (ke->key() == Qt::Key_Space && !ke->isAutoRepeat() &&
            qApp->focusWidget() != m_reflectorCombo->lineEdit() &&
            qApp->focusWidget() != m_connectButton) {
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

    m_connectedReflectorName = reflectorName.isEmpty() ? "?" : reflectorName;
    m_connectedModule = targetModule;

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
    addLastHeardEntry(header);
}

void MainWindow::addLastHeardEntry(const dextra::DStarHeader &header) {
    QString callsign = QString::fromStdString(header.myCall);
    if (!header.myCall2.empty()) callsign += "/" + QString::fromStdString(header.myCall2);

    m_lastHeardTable->insertRow(0);
    m_lastHeardTable->setItem(0, 0, new QTableWidgetItem(QTime::currentTime().toString("HH:mm:ss")));
    m_lastHeardTable->setItem(0, 1, new QTableWidgetItem(callsign));
    m_lastHeardTable->setItem(0, 2, new QTableWidgetItem(m_connectedReflectorName));
    m_lastHeardTable->setItem(0, 3, new QTableWidgetItem(QString(QChar(m_connectedModule))));

    // Newest-first log, capped rather than left to grow unbounded over a
    // long session -- the oldest entries are at the bottom.
    constexpr int kMaxLastHeardRows = 100;
    while (m_lastHeardTable->rowCount() > kMaxLastHeardRows) {
        m_lastHeardTable->removeRow(m_lastHeardTable->rowCount() - 1);
    }
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
    m_reflectorCombo->setEnabled(!busy && !m_connected);
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
