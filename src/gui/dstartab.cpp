#include "dstartab.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QCompleter>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QShortcut>
#include <QSplitter>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

#include "audio_gain.h"
#include "audiolevelspanel.h"
#include "reflectorlistmodel.h"

namespace {
// Button/label colors are set explicitly (rather than left to the system
// theme) specifically to be visible cues independent of it -- white text
// reads fine on both regardless of light/dark mode.
const char *kConnectedButtonStyle = "background-color: #4CAF50; color: white;";
const char *kErrorButtonStyle = "background-color: #f44336; color: white;";
const char *kErrorLabelStyle = "color: #f44336;";
// Mid-dark amber/cyan (not the pure hues) so they stay readable on both light and dark window themes.
const char *kStatusLabelStyle = "color: #b36b00;";
// Distinct from the error red above -- an "on-air" orange, deliberately
// hard to miss so a forgotten toggled-on transmit is obvious at a glance.
const char *kSendingButtonStyle = "background-color: #ff9800; color: white;";
// Lighter than plain white (easier on the eyes) but distinct from the
// metallic window background so content areas still read as content.
const char *kLightGrayBackground = "#e8e8e8";

constexpr int kFavouriteNameRole = Qt::UserRole;
constexpr int kFavouriteModuleRole = Qt::UserRole + 1;

QString favouritesFilePath() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + "/dstar_favourites.json";
}

QString favouriteText(const QString &reflectorName, char module) {
    return reflectorName + " \u2013 module " + QChar(module);
}
} // namespace

DStarTab::DStarTab(const GuiSettings &settings, QWidget *parent) : ProtocolTab(parent), m_settings(settings) {
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
    connect(m_reflectorCombo->lineEdit(), &QLineEdit::textChanged, this, &DStarTab::updateConnectButtonEnabled);

    connect(m_reflectorCombo, &QComboBox::currentIndexChanged, this, &DStarTab::updateConnectButtonEnabled);

    // Index 0 is a placeholder, not a real module -- without it the combo
    // box would silently start on "A" already selected, letting Connect
    // send you to a module you never actually chose.
    m_targetModule = new QComboBox;
    m_targetModule->addItem("Select module...");
    for (char c = 'A'; c <= 'Z'; c++) m_targetModule->addItem(QString(QChar(c)));
    connect(m_targetModule, &QComboBox::currentIndexChanged, this, &DStarTab::updateConnectButtonEnabled);

    m_connectButton = new QPushButton("Connect");
    connect(m_connectButton, &QPushButton::clicked, this, &DStarTab::onConnectClicked);

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
    m_statusLabel->setStyleSheet(kStatusLabelStyle);
    // A QLabel's minimum width is its full text width, so a long status
    // would raise the left panel's minimum and squeeze the splitter.
    // Ignored lets it clip instead of dictating the panel's width.
    m_statusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

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

    m_addFavouriteButton = new QPushButton("Add to favourites");
    m_addFavouriteButton->setEnabled(false);
    connect(m_addFavouriteButton, &QPushButton::clicked, this, [this] {
        addFavouriteItem(selectedReflectorName(), static_cast<char>('A' + m_targetModule->currentIndex() - 1));
        saveFavourites();
        updateAddFavouriteEnabled();
    });

    // Picking an entry loads its reflector and module into the combos, as
    // if chosen by hand. itemClicked rather than currentItemChanged so
    // re-clicking the already-current entry still applies it.
    m_favouritesList = new QListWidget;
    m_favouritesList->setStyleSheet(QString("QListWidget { background-color: %1; }").arg(kLightGrayBackground));
    auto applyFavourite = [this](QListWidgetItem *item) {
        selectFavourite(item->data(kFavouriteNameRole).toString(),
                        static_cast<char>(item->data(kFavouriteModuleRole).toInt()));
    };
    connect(m_favouritesList, &QListWidget::itemClicked, this, applyFavourite);
    connect(m_favouritesList, &QListWidget::itemActivated, this, applyFavourite);

    // Remove via right-click menu or the Delete key.
    m_favouritesList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_favouritesList, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QListWidgetItem *item = m_favouritesList->itemAt(pos);
        if (!item) return;
        QMenu menu;
        QAction *remove = menu.addAction("Remove from favourites");
        if (menu.exec(m_favouritesList->viewport()->mapToGlobal(pos)) == remove) removeFavouriteItem(item);
    });
    auto *deleteShortcut = new QShortcut(QKeySequence::Delete, m_favouritesList);
    deleteShortcut->setContext(Qt::WidgetShortcut);
    connect(deleteShortcut, &QShortcut::activated, this, [this] {
        if (QListWidgetItem *item = m_favouritesList->currentItem()) removeFavouriteItem(item);
    });

    auto *moduleRow = new QHBoxLayout;
    moduleRow->addWidget(new QLabel("Target module:"));
    moduleRow->addWidget(m_targetModule);
    moduleRow->addStretch();
    moduleRow->addWidget(m_addFavouriteButton);

    auto *bottomRow = new QHBoxLayout;
    bottomRow->addWidget(m_connectButton);
    bottomRow->addWidget(m_statusLabel, 1);
    bottomRow->addWidget(m_pttButton);

    auto *layout = new QVBoxLayout;
    layout->addWidget(m_reflectorCombo);
    layout->addLayout(moduleRow);
    layout->addLayout(bottomRow);
    layout->addWidget(new QLabel("Favourites:"));
    layout->addWidget(m_favouritesList, 1);
    layout->addWidget(headerBox);

    // Mic/speaker volume, at the bottom so it's always in reach mid-QSO.
    // Forwarded up (volumesChanged) so MainWindow can keep the DMR tab's
    // sliders and the saved settings in step.
    m_audioLevels = new AudioLevelsPanel;
    connect(m_audioLevels, &AudioLevelsPanel::changed, this, [this](int mic, int speaker) {
        setVolumes(mic, speaker);
        emit volumesChanged(mic, speaker);
    });
    connect(m_audioLevels, &AudioLevelsPanel::committed, this, &ProtocolTab::volumesCommitted);
    layout->addWidget(m_audioLevels);

    // Level meters: peak of what the mic device is delivering and what's
    // going to the speaker (both after gain), polled from the AlsaPcm
    // objects. Idle when disconnected -- nothing reads or writes them, so
    // the bars just fall to zero. The mic meter only shows while PTT is
    // down (the capture thread keeps reading between overs, but that audio
    // goes nowhere); the peak is still taken so a stale one doesn't flash
    // up the moment PTT is pressed.
    auto *levelTimer = new QTimer(this);
    connect(levelTimer, &QTimer::timeout, this, [this] {
        int mic = m_capture.takePeak();
        m_audioLevels->updateLevels(m_pttActive.load() ? mic : 0, m_playback.takePeak());
        updateVocoderStatus();
    });
    levelTimer->start(50);

    auto *leftPanel = new QWidget;
    leftPanel->setLayout(layout);

    auto *splitter = new QSplitter;
    m_splitter = splitter;
    splitter->addWidget(leftPanel);
    splitter->addWidget(m_lastHeardTable);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(splitter);

    loadFavourites();

    m_model->refresh();
    updateConnectButtonEnabled();
}

QWidget *DStarTab::spaceExemptFocusWidget() const { return m_reflectorCombo->lineEdit(); }

// currentIndex() alone isn't enough: QComboBox doesn't automatically
// invalidate it just because the user edited the line edit's text away
// from whatever that row says (there's no "unselect on edit" built in).
// So a pick only counts if the row it points to is still what's actually
// displayed right now.
bool DStarTab::hasValidReflectorSelection() const {
    int row = m_reflectorCombo->currentIndex();
    if (row < 0) return false;
    return m_reflectorCombo->currentText() == m_model->data(m_model->index(row, 0), Qt::DisplayRole).toString();
}

QString DStarTab::selectedHost() const {
    if (!hasValidReflectorSelection()) return {};
    return m_model->data(m_model->index(m_reflectorCombo->currentIndex(), 0), ReflectorListModel::HostRole).toString();
}

QString DStarTab::selectedReflectorName() const {
    if (!hasValidReflectorSelection()) return {};
    return m_model->data(m_model->index(m_reflectorCombo->currentIndex(), 0), ReflectorListModel::NameRole).toString();
}

void DStarTab::onConnectClicked() {
    if (m_busy) return;
    if (m_connected) {
        startDisconnect();
    } else {
        startConnect();
    }
}

void DStarTab::startConnect() {
    QString host = selectedHost();
    if (host.isEmpty()) {
        QMessageBox::information(this, "No reflector selected", "Pick a reflector from the list first.");
        return;
    }
    if (m_targetModule->currentIndex() <= 0) {
        QMessageBox::information(this, "No module selected", "Pick a target module first.");
        return;
    }
    if (m_settings.thumbdvTarget().isEmpty()) {
        QMessageBox::warning(this, "No ThumbDV device", "Set your ThumbDV (serial device or AMBEServer host) in Settings first.");
        return;
    }

    QString reflectorName = selectedReflectorName();
    char targetModule = m_targetModule->currentText().at(0).toLatin1();
    m_connectButton->setStyleSheet("");
    m_statusLabel->setStyleSheet(kStatusLabelStyle);
    setBusy(true, "Connecting to " + host + "...");

    if (m_worker.joinable()) m_worker.join();
    m_worker = std::thread(&DStarTab::connectWorker, this, host, reflectorName, targetModule, m_settings);
}

void DStarTab::connectWorker(QString host, QString reflectorName, char targetModule, GuiSettings settings) {
    QString error;
    bool ok = true;

    m_dv = std::make_unique<SerialDV::DVController>();
    if (!m_dv->open(settings.thumbdvTarget().toStdString())) {
        error = "Failed to open ThumbDV " + settings.thumbdvTarget();
        ok = false;
    } else {
        checkThumbdvLatency(settings);
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
        m_client->setIdentity(settings.callsign.toStdString(), myModule, settings.suffix.toStdString());
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

void DStarTab::onConnectFinished(bool ok, QString error, QString reflectorName, char targetModule) {
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
    m_playbackThread = std::thread(audio::playbackThread, &m_playback, &m_rxQueue, &dextra::g_running);
    m_networkThread = std::thread([this] { m_client->run(); });

    setConnected(true, QString("Connected to %1, module %2")
                            .arg(reflectorName.isEmpty() ? "?" : reflectorName)
                            .arg(QChar(targetModule)));
}

void DStarTab::startDisconnect() {
    setBusy(true, "Disconnecting...");
    if (m_worker.joinable()) m_worker.join();
    m_worker = std::thread(&DStarTab::disconnectWorker, this);
}

void DStarTab::disconnectWorker() {
    stopSessionBlocking();
    QMetaObject::invokeMethod(this, [this]() { onDisconnectFinished(); }, Qt::QueuedConnection);
}

void DStarTab::onDisconnectFinished() {
    m_client.reset();
    m_dv.reset();
    m_rpt1Label->setText("—");
    m_rpt2Label->setText("—");
    m_urCallLabel->setText("—");
    m_myCallLabel->setText("—");
    m_myCall2Label->setText("—");
    setConnected(false, "Disconnected.");
}

void DStarTab::onHeaderReceived(dextra::DStarHeader header) {
    m_rpt1Label->setText(QString::fromStdString(header.rpt1));
    m_rpt2Label->setText(QString::fromStdString(header.rpt2));
    m_urCallLabel->setText(QString::fromStdString(header.urCall));
    m_myCallLabel->setText(QString::fromStdString(header.myCall));
    m_myCall2Label->setText(QString::fromStdString(header.myCall2));
    addLastHeardEntry(header);
}

void DStarTab::addLastHeardEntry(const dextra::DStarHeader &header) {
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

void DStarTab::stopSessionBlocking() {
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

// Applied to the AlsaPcm objects themselves, which outlive any one
// connection -- so this works while disconnected too and carries over to
// the next connect.
void DStarTab::setVolumes(int mic, int speaker) {
    m_audioLevels->setVolumes(mic, speaker);
    m_capture.setGain(sliderToGain(mic));
    m_playback.setGain(sliderToGain(speaker));
}

void DStarTab::setBusy(bool busy, const QString &status) {
    m_busy = busy;
    m_reflectorCombo->setEnabled(!busy && !m_connected);
    m_targetModule->setEnabled(!busy && !m_connected);
    m_statusLabel->setText(status);
    updateConnectButtonEnabled();
    emit stateChanged();
}

void DStarTab::setConnected(bool connected, const QString &status) {
    m_connected = connected;
    m_connectedStatus = connected ? status : QString();
    m_vocoderDown = false;
    m_connectButton->setText(connected ? "Disconnect" : "Connect");
    m_connectButton->setStyleSheet(connected ? kConnectedButtonStyle : "");
    m_statusLabel->setStyleSheet(kStatusLabelStyle);
    m_pttButton->setEnabled(connected);
    if (!connected) m_pttButton->setChecked(false); // in case we disconnected mid-send
    setBusy(false, status);
}

void DStarTab::updateVocoderStatus() {
    // m_dv is only created and destroyed around connect/disconnect, and
    // m_connected is true (with m_busy false) only in between, both set
    // on this thread -- so while that holds it's safe to read here.
    if (!m_connected || m_busy || !m_dv) return;
    bool down = !m_dv->isResponding();
    if (down == m_vocoderDown) return;
    m_vocoderDown = down;
    m_statusLabel->setText(down ? m_connectedStatus + " — ThumbDV not responding" : m_connectedStatus);
    m_statusLabel->setStyleSheet(down ? kErrorLabelStyle : kStatusLabelStyle);
}

void DStarTab::updateConnectButtonEnabled() {
    // Once connected, the button becomes Disconnect and should always stay
    // usable regardless of what's selected in the (by then disabled)
    // reflector list and module combo.
    bool readyToConnect = !selectedHost().isEmpty() && m_targetModule->currentIndex() > 0;
    m_connectButton->setEnabled(!m_busy && (m_connected || readyToConnect));
    updateAddFavouriteEnabled();
}

void DStarTab::updateAddFavouriteEnabled() {
    if (!m_addFavouriteButton) return; // a selection signal fired mid-construction
    bool ready = !selectedHost().isEmpty() && m_targetModule->currentIndex() > 0;
    m_addFavouriteButton->setEnabled(
        ready && !isFavourite(selectedReflectorName(), static_cast<char>('A' + m_targetModule->currentIndex() - 1)));
}

bool DStarTab::isFavourite(const QString &reflectorName, char module) const {
    for (int i = 0; i < m_favouritesList->count(); i++) {
        const QListWidgetItem *item = m_favouritesList->item(i);
        if (item->data(kFavouriteNameRole).toString() == reflectorName &&
            item->data(kFavouriteModuleRole).toInt() == module)
            return true;
    }
    return false;
}

void DStarTab::addFavouriteItem(const QString &reflectorName, char module) {
    if (reflectorName.isEmpty() || module < 'A' || module > 'Z' || isFavourite(reflectorName, module)) return;
    auto *item = new QListWidgetItem(favouriteText(reflectorName, module));
    item->setData(kFavouriteNameRole, reflectorName);
    item->setData(kFavouriteModuleRole, static_cast<int>(module));
    m_favouritesList->addItem(item);
}

void DStarTab::removeFavouriteItem(QListWidgetItem *item) {
    delete m_favouritesList->takeItem(m_favouritesList->row(item));
    saveFavourites();
    updateAddFavouriteEnabled();
}

void DStarTab::selectFavourite(const QString &reflectorName, char module) {
    // The combos are disabled while connecting/connected -- don't change
    // the selection out from under a live session.
    if (!m_reflectorCombo->isEnabled()) return;
    for (int row = 0; row < m_model->rowCount(); row++) {
        if (m_model->data(m_model->index(row, 0), ReflectorListModel::NameRole).toString() == reflectorName) {
            m_reflectorCombo->setCurrentIndex(row);
            m_targetModule->setCurrentIndex(module - 'A' + 1);
            return;
        }
    }
    // Not in the current directory (removed since, or the list hasn't
    // loaded yet) -- say so rather than silently doing nothing.
    m_statusLabel->setText(reflectorName + " isn't in the reflector list");
}

void DStarTab::loadFavourites() {
    QFile f(favouritesFilePath());
    if (!f.open(QIODevice::ReadOnly)) return;
    for (const QJsonValue &v : QJsonDocument::fromJson(f.readAll()).array()) {
        QJsonObject o = v.toObject();
        QString module = o["module"].toString();
        if (module.size() == 1) addFavouriteItem(o["reflector"].toString(), module.at(0).toLatin1());
    }
}

void DStarTab::saveFavourites() const {
    QJsonArray array;
    for (int i = 0; i < m_favouritesList->count(); i++) {
        const QListWidgetItem *item = m_favouritesList->item(i);
        QJsonObject o;
        o["reflector"] = item->data(kFavouriteNameRole).toString();
        o["module"] = QString(QChar(item->data(kFavouriteModuleRole).toInt()));
        array.append(o);
    }
    QFile f(favouritesFilePath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
}
