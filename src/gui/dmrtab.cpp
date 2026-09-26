#include "dmrtab.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QShortcut>
#include <QSplitter>
#include <QTimer>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTime>
#include <QVBoxLayout>

#include <cstdio>
#include <thread>

#include "audio_gain.h"
#include "audiolevelspanel.h"
#include "dmriddirectory.h"
#include "localcache.h"
#include "talkgrouplistmodel.h"

namespace {
// Same palette as DStarTab -- kept as a separate copy rather than shared
// constants, since the two tabs are otherwise independent and this is the
// only thing they'd share.
const char *kConnectedButtonStyle = "background-color: #4CAF50; color: white;";
const char *kErrorButtonStyle = "background-color: #f44336; color: white;";
const char *kErrorLabelStyle = "color: #f44336;";
// Mid-dark amber/cyan (not the pure hues) so they stay readable on both light and dark window themes.
const char *kStatusLabelStyle = "color: #b36b00;";
const char *kSubscriptionLabelStyle = "color: #00838f;";
const char *kSendingButtonStyle = "background-color: #ff9800; color: white;";
const char *kLightGrayBackground = "#e8e8e8";

constexpr int kFavouriteIdRole = Qt::UserRole;
constexpr int kFavouritePrivateRole = Qt::UserRole + 1;

QString favouritesFilePath() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + "/dmr_favourites.json";
}
} // namespace

DmrTab::DmrTab(const GuiSettings &settings, QWidget *parent) : ProtocolTab(parent), m_settings(settings) {
    m_talkgroupModel = new TalkgroupListModel(this);
    connect(m_talkgroupModel, &TalkgroupListModel::refreshFailed, this, [this](const QString &error) {
        m_statusLabel->setText("Talkgroup list: " + error + " (showing built-in list only)");
    });

    // Same editable-combo-with-completer pattern as DStarTab's reflector
    // picker: typing narrows a popup of matches (by number or name
    // substring -- "530" and "zealand" both find "5302 — ZL2 Regional"),
    // picking one fills the line edit. Unlike the reflector combo, a typed
    // value that matches nothing listed is still perfectly valid here (see
    // currentTalkgroupId()), so there's no equivalent of
    // hasValidReflectorSelection() gating anything.
    m_talkgroupCombo = new QComboBox;
    m_talkgroupCombo->setEditable(true);
    m_talkgroupCombo->setInsertPolicy(QComboBox::NoInsert);
    // Without this, the combo's width follows its content (QComboBox's
    // default AdjustToContentsOnFirstShow policy), which left the
    // DMR/Last-Heard splitter's left panel exactly at its minimum width --
    // no room to shrink left (it looked stuck, then snapped straight to
    // fully collapsed). A fixed contents-length keeps the combo's width
    // independent of what's in the directory.
    m_talkgroupCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_talkgroupCombo->setMinimumContentsLength(15);
    // setMinimumContentsLength() alone only bounds the combo's *preferred*
    // width -- under Qt's default sizing, a QComboBox's minimumSizeHint()
    // tracks that same preferred width 1:1 (no smaller floor exists on its
    // own), which is what left the splitter with zero slack to shrink into
    // in the first place. An explicit, genuinely small minimum width
    // overrides that and gives the splitter real room to shrink left (the
    // combo just clips/scrolls its text at small widths, same as any
    // shrunk QLineEdit).
    m_talkgroupCombo->setMinimumWidth(60);
    m_talkgroupCombo->setModel(m_talkgroupModel);
    m_talkgroupCombo->setCurrentIndex(-1);
    m_talkgroupCombo->lineEdit()->setPlaceholderText("Talkgroup, e.g. 91 (World-wide) or a number/name to search...");
    m_talkgroupCombo->setStyleSheet(
        QString("QComboBox QAbstractItemView { background-color: %1; }").arg(kLightGrayBackground));

    // See ReflectorListModel's identical connection for why: QComboBox
    // resets currentIndex to row 0 on its own whenever its model resets,
    // which would otherwise silently "select" row 0 the moment the live
    // directory fetch replaces the static fallback rows.
    connect(m_talkgroupModel, &QAbstractItemModel::modelReset, this, [this] { m_talkgroupCombo->setCurrentIndex(-1); });

    auto *completer = new QCompleter(m_talkgroupModel, this);
    completer->setCompletionRole(Qt::DisplayRole);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    completer->popup()->setStyleSheet(QString("background-color: %1;").arg(kLightGrayBackground));
    m_talkgroupCombo->setCompleter(completer);

    connect(m_talkgroupCombo->lineEdit(), &QLineEdit::textChanged, this, [this] {
        m_talkgroup.store(currentTalkgroupId());
        updatePttButtonEnabled();
    });

    m_talkgroupModel->refresh();

    // Group vs Private call -- see the header comment on m_privateCallCheck.
    // When checked, the combo above is read as a target DMR ID instead of
    // a talkgroup; the placeholder text updates to make that clear.
    m_privateCallCheck = new QCheckBox("Private call");
    connect(m_privateCallCheck, &QCheckBox::toggled, this, [this](bool privateCall) {
        m_privateCall.store(privateCall);
        m_talkgroupCombo->lineEdit()->setPlaceholderText(
            privateCall ? "Target DMR ID, e.g. 9990 (BrandMeister Parrot echo test)"
                        : "Talkgroup, e.g. 91 (World-wide) or a number/name to search...");
        // Deliberately not resubscribing here -- see resubscribeIfOpenTerminal().
    });

    m_connectButton = new QPushButton("Connect");
    connect(m_connectButton, &QPushButton::clicked, this, &DmrTab::onConnectClicked);

    // Toggle, not press-and-hold -- see DStarTab's identical PTT button
    // for the reasoning (a mouse button held down for a whole
    // transmission was awkward in practice).
    m_pttButton = new QPushButton("PTT to send");
    m_pttButton->setEnabled(false);
    m_pttButton->setCheckable(true);
    connect(m_pttButton, &QPushButton::toggled, this, [this](bool sending) {
        m_pttActive.store(sending);
        m_pttButton->setText(sending ? "Sending (click to stop)" : "PTT to send");
        m_pttButton->setStyleSheet(sending ? kSendingButtonStyle : "");
        // Captured here (GUI thread, at the exact moment PTT goes down)
        // rather than read back from the capture thread -- at this instant
        // the combo box's current value is exactly what captureThread is
        // about to read and use for this transmission (see m_talkgroup's
        // own comment on why the two threads don't share the widget
        // directly). Stays showing this after PTT releases -- see
        // m_activeTalkgroupLabel's comment for why that's the right
        // "current TG" to keep displayed.
        if (sending) {
            setActiveSubscription(currentTalkgroupId(), m_privateCall.load());
            // Open DMR Terminal only -- PTT is an explicit action, so
            // (unlike typing in the combo) it's a reasonable trigger to
            // also make sure RX follows whatever was just keyed up to.
            resubscribeIfOpenTerminal();
        }
    });

    m_statusLabel = new QLabel("Disconnected.");
    m_statusLabel->setStyleSheet(kStatusLabelStyle);
    m_activeTalkgroupLabel = new QLabel("Current subscription: None");
    m_activeTalkgroupLabel->setStyleSheet(kSubscriptionLabelStyle);
    // A QLabel's minimum width is its full text width, so a long status
    // ("Connected to 5051.master...") or subscription line would raise the
    // left panel's minimum and squeeze the splitter. Ignored lets these
    // clip instead of dictating the panel's width.
    m_statusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_activeTalkgroupLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    m_unsubscribeButton = new QPushButton("Unsubscribe");
    m_unsubscribeButton->setEnabled(false);
    m_unsubscribeButton->setFixedWidth(m_unsubscribeButton->sizeHint().width());
    connect(m_unsubscribeButton, &QPushButton::clicked, this, &DmrTab::unsubscribeCurrent);

    m_addFavouriteButton = new QPushButton("Add to favourites");
    m_addFavouriteButton->setEnabled(false);
    m_addFavouriteButton->setFixedWidth(m_addFavouriteButton->sizeHint().width());
    connect(m_addFavouriteButton, &QPushButton::clicked, this, [this] {
        addFavouriteItem(m_activeId, m_activePrivate);
        saveFavourites();
        updateAddFavouriteEnabled();
    });

    // Picking an entry loads it into the talkgroup combo (and the Private
    // call checkbox) so PTT uses it. itemClicked rather than currentItem
    // Changed so re-clicking the already-current entry still applies it.
    m_favouritesList = new QListWidget;
    m_favouritesList->setStyleSheet(QString("QListWidget { background-color: %1; }").arg(kLightGrayBackground));
    auto applyFavourite = [this](QListWidgetItem *item) {
        selectFavourite(item->data(kFavouriteIdRole).toUInt(), item->data(kFavouritePrivateRole).toBool());
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

    // Callsign column shows "CALLSIGN (id)" via m_dmrIdDirectory (fetched
    // below), or just the id if that lookup hasn't loaded yet or doesn't
    // have this particular one. Talkgroup column similarly resolves a name
    // via m_talkgroupModel -- see displayCallsign()/displayTalkgroup().
    m_lastHeardTable = new QTableWidget(0, 3);
    m_lastHeardTable->setHorizontalHeaderLabels({"Time", "Callsign", "Talkgroup"});
    m_lastHeardTable->verticalHeader()->setVisible(false);
    m_lastHeardTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_lastHeardTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_lastHeardTable->setFocusPolicy(Qt::NoFocus); // a passive log, not a tab stop
    m_lastHeardTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_lastHeardTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_lastHeardTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_lastHeardTable->setStyleSheet(QString("QTableWidget { background-color: %1; }").arg(kLightGrayBackground));

    auto *tgRow = new QHBoxLayout;
    tgRow->addWidget(new QLabel("Talkgroup:"));
    tgRow->addWidget(m_talkgroupCombo, 1);
    tgRow->addWidget(m_privateCallCheck);

    auto *bottomRow = new QHBoxLayout;
    bottomRow->addWidget(m_connectButton);
    bottomRow->addWidget(m_statusLabel, 1);
    bottomRow->addWidget(m_pttButton);

    auto *layout = new QVBoxLayout;
    layout->addLayout(tgRow);
    auto *subscriptionRow = new QHBoxLayout;
    subscriptionRow->addWidget(m_activeTalkgroupLabel, 1);
    subscriptionRow->addWidget(m_unsubscribeButton);
    subscriptionRow->addWidget(m_addFavouriteButton);
    layout->addLayout(subscriptionRow);
    layout->addLayout(bottomRow);
    layout->addWidget(new QLabel("Favourites:"));
    layout->addWidget(m_favouritesList, 1);

    // Mic/speaker volume, at the bottom so it's always in reach mid-QSO.
    // Forwarded up (volumesChanged) so MainWindow can keep the D-Star
    // tab's sliders and the saved settings in step.
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
    // the bars just fall to zero.
    auto *levelTimer = new QTimer(this);
    connect(levelTimer, &QTimer::timeout, this,
            [this] { m_audioLevels->updateLevels(m_capture.takePeak(), m_playback.takePeak()); });
    levelTimer->start(50);

    auto *leftPanel = new QWidget;
    leftPanel->setLayout(layout);

    // Same left-panel/Last-Heard split as DStarTab, for a consistent look
    // across the two protocol tabs.
    auto *splitter = new QSplitter;
    splitter->addWidget(leftPanel);
    splitter->addWidget(m_lastHeardTable);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(splitter);

    // Load last run's cached DMR ID directory synchronously -- fast (it's
    // a local file), so Last Heard can resolve callsigns immediately
    // instead of waiting on the ~330k-line network fetch below to finish.
    dmr::loadCachedDmrIdDirectory(m_dmrIdDirectory);

    loadFavourites();

    // One-shot background fetch of the live DMR ID directory (see
    // dmriddirectory.h), replacing the cached copy above once it lands
    // and refreshing the on-disk cache for next startup -- skipped
    // entirely if the cache is still fresh (matches Pi-Star's own 24h
    // reload interval for this exact directory), so this multi-megabyte
    // fetch doesn't happen on every single launch.
    if (!dmr::isDmrIdDirectoryCacheFresh(cache::ONE_DAY_SECONDS)) {
        std::thread([this] {
            QHash<uint32_t, QString> directory;
            QString error;
            bool ok = dmr::fetchDmrIdDirectory(directory, error);
            QMetaObject::invokeMethod(
                this,
                [this, ok, directory = std::move(directory), error]() mutable {
                    if (ok) {
                        m_dmrIdDirectory = std::move(directory);
                    } else {
                        std::fprintf(stderr, "dmrtab: DMR ID directory fetch failed: %s\n", error.toUtf8().constData());
                    }
                },
                Qt::QueuedConnection);
        }).detach();
    }
}

QWidget *DmrTab::spaceExemptFocusWidget() const { return m_talkgroupCombo->lineEdit(); }

QString DmrTab::displayCallsign(uint32_t dmrId) const {
    auto it = m_dmrIdDirectory.constFind(dmrId);
    if (it == m_dmrIdDirectory.constEnd()) return QString::number(dmrId);
    return it.value() + " (" + QString::number(dmrId) + ")";
}

QString DmrTab::displayTalkgroup(uint32_t dstId) const {
    QString name = m_talkgroupModel->nameForId(dstId);
    if (name.isEmpty()) return QString::number(dstId);
    return QString::number(dstId) + " — " + name;
}

void DmrTab::setActiveSubscription(uint32_t id, bool privateCall) {
    // m_activeId keeps 4000 (BrandMeister's "disconnect" TG) so it can still
    // be added as a favourite, but the label shows it as "None" since it
    // means no subscription. displayTalkgroup() falls back to the plain
    // numeric ID when there's no directory entry, which is always the case
    // for a private-call target (a subscriber ID, not a talkgroup).
    m_activeId = id;
    m_activePrivate = id != 0 && privateCall;
    bool none = id == 0 || id == 4000;
    m_activeTalkgroupLabel->setText(
        "Current subscription: " + (none ? QString("None") : displayTalkgroup(id) + (privateCall ? " (private call)" : "")));
    updateAddFavouriteEnabled();
    updateUnsubscribeButtonEnabled();
}

void DmrTab::updateAddFavouriteEnabled() {
    m_addFavouriteButton->setEnabled(m_activeId != 0 && !isFavourite(m_activeId, m_activePrivate));
}

bool DmrTab::isFavourite(uint32_t id, bool privateCall) const {
    for (int i = 0; i < m_favouritesList->count(); i++) {
        const QListWidgetItem *item = m_favouritesList->item(i);
        if (item->data(kFavouriteIdRole).toUInt() == id && item->data(kFavouritePrivateRole).toBool() == privateCall)
            return true;
    }
    return false;
}

void DmrTab::addFavouriteItem(uint32_t id, bool privateCall) {
    if (id == 0 || isFavourite(id, privateCall)) return;
    QString text = displayTalkgroup(id);
    if (id == 4000 && text == "4000") text += " — Disconnect";
    auto *item = new QListWidgetItem(text + (privateCall ? " (private call)" : ""));
    item->setData(kFavouriteIdRole, id);
    item->setData(kFavouritePrivateRole, privateCall);
    m_favouritesList->addItem(item);
}

void DmrTab::removeFavouriteItem(QListWidgetItem *item) {
    delete m_favouritesList->takeItem(m_favouritesList->row(item));
    saveFavourites();
    updateAddFavouriteEnabled();
}

void DmrTab::selectFavourite(uint32_t id, bool privateCall) {
    m_privateCallCheck->setChecked(privateCall);
    m_talkgroupCombo->lineEdit()->setText(privateCall ? QString::number(id) : displayTalkgroup(id));
}

void DmrTab::loadFavourites() {
    QFile f(favouritesFilePath());
    if (!f.open(QIODevice::ReadOnly)) return;
    for (const QJsonValue &v : QJsonDocument::fromJson(f.readAll()).array()) {
        QJsonObject o = v.toObject();
        addFavouriteItem(static_cast<uint32_t>(o["id"].toDouble()), o["private"].toBool());
    }
}

void DmrTab::saveFavourites() const {
    QJsonArray array;
    for (int i = 0; i < m_favouritesList->count(); i++) {
        const QListWidgetItem *item = m_favouritesList->item(i);
        QJsonObject o;
        o["id"] = static_cast<double>(item->data(kFavouriteIdRole).toUInt());
        o["private"] = item->data(kFavouritePrivateRole).toBool();
        array.append(o);
    }
    QFile f(favouritesFilePath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
}

uint32_t DmrTab::currentTalkgroupId() const {
    QString text = m_talkgroupCombo->currentText();
    int i = 0;
    while (i < text.size() && !text.at(i).isDigit()) i++;
    int start = i;
    while (i < text.size() && text.at(i).isDigit()) i++;
    if (i == start) return 0;
    return text.mid(start, i - start).toUInt();
}

void DmrTab::resubscribeIfOpenTerminal() {
    if (!m_rewindClient) return;
    uint32_t id = currentTalkgroupId();
    if (id == 0) return;
    bool privateCall = m_privateCall.load();
    if (id == m_subscribedTalkgroup && privateCall == m_subscribedPrivate) return;

    if (m_subscribedTalkgroup != 0) {
        m_rewindClient->unsubscribe(m_subscribedTalkgroup, m_subscribedPrivate ? dmr::rewind::SessionType::PrivateVoice
                                                                                 : dmr::rewind::SessionType::GroupVoice);
    }
    m_rewindClient->subscribe(id, privateCall ? dmr::rewind::SessionType::PrivateVoice
                                               : dmr::rewind::SessionType::GroupVoice);
    m_subscribedTalkgroup = id;
    m_subscribedPrivate = privateCall;
    // Unlike Homebrew (where this label tracks the last-transmitted TG,
    // since that's the only thing that actually determines RX there),
    // Open DMR Terminal's subscription genuinely is the current RX
    // target -- update the label to match.
    setActiveSubscription(id, privateCall);
}

void DmrTab::onConnectClicked() {
    if (m_busy) return;
    if (m_connected) {
        startDisconnect();
    } else {
        startConnect();
    }
}

void DmrTab::startConnect() {
    bool openTerminal = m_settings.dmrProtocol == "opendmr";
    QString server = openTerminal ? m_settings.dmrOpenTerminalServer : m_settings.dmrServer;
    if (server.isEmpty()) {
        QMessageBox::information(this, "No DMR server",
                                  QString("Set a %1 server (host:port) in Settings first.")
                                      .arg(openTerminal ? "Open DMR Terminal" : "DMR"));
        return;
    }
    if (m_settings.dmrId == 0) {
        QMessageBox::information(this, "No DMR ID", "Set your DMR ID in Settings first.");
        return;
    }
    if (m_settings.thumbdvDevice.isEmpty()) {
        QMessageBox::warning(this, "No ThumbDV device", "Set your ThumbDV device in Settings first.");
        return;
    }

    m_connectButton->setStyleSheet("");
    m_statusLabel->setStyleSheet(kStatusLabelStyle);
    setBusy(true, "Connecting to " + server + "...");

    // Captured here (GUI thread) rather than read from connectWorker --
    // the combo box/checkbox aren't safe to touch off the GUI thread.
    // Only used for Open DMR Terminal's initial subscribe(); Homebrew
    // ignores these.
    uint32_t initialTalkgroup = currentTalkgroupId();
    bool initialPrivate = m_privateCall.load();

    if (m_worker.joinable()) m_worker.join();
    m_worker = std::thread(&DmrTab::connectWorker, this, m_settings, initialTalkgroup, initialPrivate);
}

void DmrTab::connectWorker(GuiSettings settings, uint32_t initialTalkgroup, bool initialPrivate) {
    QString error;
    bool ok = true;
    bool openTerminal = settings.dmrProtocol == "opendmr";

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

    QString server = openTerminal ? settings.dmrOpenTerminalServer : settings.dmrServer;
    QString host = server;
    uint16_t port = openTerminal ? dmr::rewind::DEFAULT_PORT : dmr::DEFAULT_PORT;
    int colonIndex = host.lastIndexOf(':');
    if (colonIndex >= 0) {
        port = static_cast<uint16_t>(host.mid(colonIndex + 1).toUInt());
        host = host.left(colonIndex);
    }

    // headerSink/voiceRxSink are set identically either way, through the
    // shared DmrTransport interface -- only setup (open/identity/link)
    // differs enough between the two protocols to need its own branch.
    auto headerSink = [this](uint32_t srcId, uint32_t dstId) {
        // Runs on the network thread once client->run() starts -- marshal
        // to the GUI thread rather than touching widgets directly here.
        QMetaObject::invokeMethod(
            this, [this, srcId, dstId]() { onHeaderReceived(srcId, dstId); }, Qt::QueuedConnection);
    };

    dmr::LinkResult result = dmr::LinkResult::Timeout;

    if (ok && openTerminal) {
        auto client = std::make_unique<dmr::rewind::RewindClient>();
        if (!client->open(host.toStdString(), port)) {
            error = "Failed to open network socket to " + server;
            ok = false;
        } else {
            client->setIdentity(settings.dmrId, settings.dmrPassword.toStdString(), "DV3000Client");
            client->setVoiceRxSink(dmr::makeVoiceRxHandler(m_dv.get(), &m_rxQueue));
            client->setHeaderSink(headerSink);
            result = client->link();
            if (result != dmr::LinkResult::Success) {
                error = QString("Master rejected the connection: %1").arg(dmr::ToString(result));
                ok = false;
            } else if (initialTalkgroup != 0) {
                client->subscribe(initialTalkgroup, initialPrivate ? dmr::rewind::SessionType::PrivateVoice
                                                                     : dmr::rewind::SessionType::GroupVoice);
                m_subscribedTalkgroup = initialTalkgroup;
                m_subscribedPrivate = initialPrivate;
            }
            m_rewindClient = client.get();
            m_client = std::move(client);
        }
    } else if (ok) {
        auto client = std::make_unique<dmr::DmrClient>();
        if (!client->open(host.toStdString(), port)) {
            error = "Failed to open network socket to " + server;
            ok = false;
        } else {
            dmr::RepeaterConfig config;
            config.callsign = settings.callsign.toStdString();
            config.colorCode = settings.dmrColorCode;
            config.timeSlot = settings.dmrTimeSlot == 1 ? dmr::TimeSlot::Slot1 : dmr::TimeSlot::Slot2;
            config.description = settings.dmrDescription.toStdString();
            config.url = settings.dmrUrl.toStdString();
            auto freqHz = static_cast<uint32_t>(settings.dmrFrequencyMhz * 1000000.0);
            config.rxFrequencyHz = freqHz;
            config.txFrequencyHz = freqHz; // simplex -- see settings.h's dmrFrequencyMhz comment
            config.latitude = static_cast<float>(settings.dmrLatitude);
            config.longitude = static_cast<float>(settings.dmrLongitude);
            config.location = settings.dmrLocation.toStdString();
            // 9-digit repeater ID (dmrId + 2-digit suffix) when a suffix is
            // set, so this instance can run alongside another client under
            // the same DMR ID -- see GuiSettings::dmrIdSuffix. Blank/invalid
            // = plain ID.
            uint32_t repeaterId = settings.dmrId;
            if (settings.dmrIdSuffix.size() == 2) repeaterId = settings.dmrId * 100 + settings.dmrIdSuffix.toUInt();
            std::fprintf(stderr, "dmrtab: repeater ID %u (DMR ID %u%s)\n", repeaterId, settings.dmrId,
                         repeaterId == settings.dmrId ? "" : " + suffix");
            client->setIdentity(settings.dmrId, settings.dmrPassword.toStdString(), config, repeaterId);
            client->setVoiceRxSink(dmr::makeVoiceRxHandler(m_dv.get(), &m_rxQueue));
            client->setHeaderSink(headerSink);
            result = client->link();
            if (result != dmr::LinkResult::Success) {
                error = QString("Master rejected the connection: %1").arg(dmr::ToString(result));
                ok = false;
            }
            m_client = std::move(client);
        }
    }

    if (!ok) {
        m_client.reset();
        m_rewindClient = nullptr;
        m_capture.close();
        m_playback.close();
        if (m_dv) m_dv->close();
        m_dv.reset();
    }

    QMetaObject::invokeMethod(
        this, [this, ok, error]() { onConnectFinished(ok, error); }, Qt::QueuedConnection);
}

void DmrTab::onConnectFinished(bool ok, QString error) {
    if (!ok) {
        setBusy(false, "Connect failed: " + error);
        m_connectButton->setStyleSheet(kErrorButtonStyle);
        m_statusLabel->setStyleSheet(kErrorLabelStyle);
        QMessageBox::warning(this, "Connect failed", error);
        return;
    }

    m_pttActive.store(false);
    dmr::g_running = 1;
    m_captureThread = std::thread(
        dmr::captureThread, m_dv.get(), &m_capture, m_client.get(), [this] { return m_talkgroup.load(); },
        [this] { return m_privateCall.load() ? dmr::CallType::Private : dmr::CallType::Group; },
        [this] { return m_pttActive.load(); });
    m_playbackThread = std::thread(dmr::playbackThread, &m_playback, &m_rxQueue);
    m_networkThread = std::thread([this] { m_client->run(); });

    bool openTerminal = m_settings.dmrProtocol == "opendmr";
    if (m_rewindClient && m_subscribedTalkgroup != 0) setActiveSubscription(m_subscribedTalkgroup, m_subscribedPrivate);
    setConnected(true, "Connected to " + (openTerminal ? m_settings.dmrOpenTerminalServer : m_settings.dmrServer));
}

void DmrTab::startDisconnect() {
    setBusy(true, "Disconnecting...");
    m_unsubscribeButton->setEnabled(false); // see stopSessionBlocking()
    if (m_worker.joinable()) m_worker.join();
    m_worker = std::thread(&DmrTab::disconnectWorker, this);
}

void DmrTab::disconnectWorker() {
    stopSessionBlocking();
    QMetaObject::invokeMethod(this, [this]() { onDisconnectFinished(); }, Qt::QueuedConnection);
}

void DmrTab::onDisconnectFinished() {
    m_client.reset();
    m_rewindClient = nullptr;
    m_subscribedTalkgroup = 0;
    m_subscribedPrivate = false;
    m_dv.reset();
    setActiveSubscription(0, false);
    setConnected(false, "Disconnected.");
}

void DmrTab::onHeaderReceived(uint32_t srcId, uint32_t dstId) { addLastHeardEntry(srcId, dstId); }

void DmrTab::addLastHeardEntry(uint32_t srcId, uint32_t dstId) {
    m_lastHeardTable->insertRow(0);
    m_lastHeardTable->setItem(0, 0, new QTableWidgetItem(QTime::currentTime().toString("HH:mm:ss")));
    m_lastHeardTable->setItem(0, 1, new QTableWidgetItem(displayCallsign(srcId)));
    m_lastHeardTable->setItem(0, 2, new QTableWidgetItem(displayTalkgroup(dstId)));

    constexpr int kMaxLastHeardRows = 100;
    while (m_lastHeardTable->rowCount() > kMaxLastHeardRows) {
        m_lastHeardTable->removeRow(m_lastHeardTable->rowCount() - 1);
    }
}

void DmrTab::stopSessionBlocking() {
    dmr::g_running = 0;
    m_pttActive.store(false);
    if (m_captureThread.joinable()) m_captureThread.join();
    if (m_playbackThread.joinable()) m_playbackThread.join();
    if (m_networkThread.joinable()) m_networkThread.join();
    // Open DMR Terminal only: drop the subscription explicitly rather
    // than relying on the server to clean it up after Close. Read here
    // without the GUI thread's involvement -- startDisconnect() disables
    // the Unsubscribe button first, and closeEvent() runs on the GUI
    // thread itself, so nothing else touches these meanwhile.
    if (m_rewindClient && m_subscribedTalkgroup != 0) {
        m_rewindClient->unsubscribe(m_subscribedTalkgroup, m_subscribedPrivate ? dmr::rewind::SessionType::PrivateVoice
                                                                                 : dmr::rewind::SessionType::GroupVoice);
    }
    if (m_client) m_client->disconnect();
    m_capture.close();
    m_playback.close();
    if (m_dv) m_dv->close();
    dmr::g_running = 1;
}

// Applied to the AlsaPcm objects themselves, which outlive any one
// connection -- so this works while disconnected too and carries over to
// the next connect.
void DmrTab::setVolumes(int mic, int speaker) {
    m_audioLevels->setVolumes(mic, speaker);
    m_capture.setGain(sliderToGain(mic));
    m_playback.setGain(sliderToGain(speaker));
}

void DmrTab::setBusy(bool busy, const QString &status) {
    m_busy = busy;
    // Talkgroup only matters once PTT is pressed (chosen dynamically per
    // transmission, not at connect time -- see the class comment), so it
    // stays editable even mid-connection, and Connect never depends on it.
    m_statusLabel->setText(status);
    m_connectButton->setEnabled(!busy);
    emit stateChanged();
}

void DmrTab::setConnected(bool connected, const QString &status) {
    m_connected = connected;
    m_connectButton->setText(connected ? "Disconnect" : "Connect");
    m_connectButton->setStyleSheet(connected ? kConnectedButtonStyle : "");
    m_statusLabel->setStyleSheet(kStatusLabelStyle);
    if (!connected) m_pttButton->setChecked(false); // in case we disconnected mid-send
    updatePttButtonEnabled();
    updateUnsubscribeButtonEnabled();
    setBusy(false, status);
}

void DmrTab::updatePttButtonEnabled() { m_pttButton->setEnabled(m_connected && currentTalkgroupId() != 0); }

void DmrTab::updateUnsubscribeButtonEnabled() {
    m_unsubscribeButton->setEnabled(m_rewindClient && m_subscribedTalkgroup != 0);
}

void DmrTab::unsubscribeCurrent() {
    if (!m_rewindClient || m_subscribedTalkgroup == 0) return;
    m_rewindClient->unsubscribe(m_subscribedTalkgroup, m_subscribedPrivate ? dmr::rewind::SessionType::PrivateVoice
                                                                             : dmr::rewind::SessionType::GroupVoice);
    m_subscribedTalkgroup = 0;
    m_subscribedPrivate = false;
    setActiveSubscription(0, false);
}
