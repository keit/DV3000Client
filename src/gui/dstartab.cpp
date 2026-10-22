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
#include <QStandardItemModel>
#include <QSplitter>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

#include "audio_gain.h"
#include "audiolevelspanel.h"
#include "dplus_client.h"
#include "reflectorlistmodel.h"
#include "theme.h"

namespace {
// Button/label colors are set explicitly (rather than left to the system
// theme) specifically to be visible cues independent of it -- white text
// reads fine on both regardless of light/dark mode.
const char *kConnectedButtonStyle = "background-color: #4CAF50; color: white;";
const char *kErrorButtonStyle = "background-color: #f44336; color: white;";
// Distinct from the error red above -- an "on-air" orange, deliberately
// hard to miss so a forgotten toggled-on transmit is obvious at a glance.
const char *kSendingButtonStyle = "background-color: #ff9800; color: white;";

constexpr int kFavouriteNameRole = Qt::UserRole;
constexpr int kFavouriteModuleRole = Qt::UserRole + 1;

QString favouritesFilePath() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + "/dstar_favourites.json";
}

QString favouriteText(const QString &reflectorName, char module) {
    return reflectorName + (module == '*' ? QString(" \u2013 all modules") : QString(" \u2013 module ") + QChar(module));
}

QString moduleText(char module) { return module == '*' ? QString("all") : QString(QChar(module)); }
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
    m_reflectorCombo->lineEdit()->setPlaceholderText("Search reflectors and gateways (e.g. 123, XLX123, XRF587, REF001, ZL2VH)...");
    m_reflectorCombo->setStyleSheet(theme::contentBackground("QComboBox QAbstractItemView"));

    // QComboBox resets currentIndex to row 0 on its own whenever its model
    // resets -- the setCurrentIndex(-1) above only covers the very first
    // setModel() call, not the *second* reset that happens moments later
    // when the async live reflector list finishes loading and
    // ReflectorListModel::applyLive() rebuilds the row list. Without this,
    // the box would silently end up with reflector #1 "selected" (and its
    // name filled into the line edit) as soon as that background fetch
    // completes, whether or not the user has touched anything yet.
    //
    // A refresh after the user has picked something (the DPlus list is
    // refreshed by every DPlus connect) mustn't lose that pick, though:
    // remember it across the reset and select the same entry again.
    connect(m_model, &QAbstractItemModel::modelAboutToBeReset, this, [this] {
        m_restoringPick = true; // the selection is in flux until it's restored below
        m_pickBeforeReset = selectedReflectorName();
        m_pickBeforeResetDPlus = selectedIsDPlus();
    });
    connect(m_model, &QAbstractItemModel::modelReset, this, [this] {
        m_reflectorCombo->setCurrentIndex(-1);
        for (int row = 0; !m_pickBeforeReset.isEmpty() && row < m_model->rowCount(); row++) {
            QModelIndex index = m_model->index(row, 0);
            if (m_model->data(index, ReflectorListModel::NameRole).toString() == m_pickBeforeReset &&
                m_model->data(index, ReflectorListModel::IsDPlusRole).toBool() == m_pickBeforeResetDPlus) {
                m_reflectorCombo->setCurrentIndex(row);
                break;
            }
        }
        m_pickBeforeReset.clear();
        m_restoringPick = false;
        updateConnectButtonEnabled();
    });

    auto *completer = new QCompleter(m_model, this);
    completer->setCompletionRole(Qt::DisplayRole);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains); // matches anywhere in the name/country/comment, not just a prefix
    completer->setCompletionMode(QCompleter::PopupCompletion);
    completer->popup()->setStyleSheet(theme::contentBackground("QAbstractItemView"));
    m_reflectorCombo->setCompleter(completer);

    // Re-check on every text change, not just when currentIndex() itself
    // changes -- see selectedRow(). textChanged (not
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
    // send you to a module you never actually chose. Each item's data is
    // its module (see selectedModule()). "All modules" plays every
    // module's traffic, like a radio's Repeater Monitor -- DPlus only,
    // since a DExtra link is to one module.
    m_targetModule = new QComboBox;
    m_targetModule->addItem("Select module...", 0);
    m_targetModule->addItem("All modules", static_cast<int>('*'));
    m_targetModule->setItemData(1, "REF reflectors and DPlus gateways only: listen to every module (no transmitting)",
                                Qt::ToolTipRole);
    for (char c = 'A'; c <= 'Z'; c++) m_targetModule->addItem(QString(QChar(c)), static_cast<int>(c));
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
    m_statusLabel->setStyleSheet(theme::statusLabelStyle());
    // A QLabel's minimum width is its full text width, so a long status
    // would raise the left panel's minimum and squeeze the splitter.
    // Ignored lets it clip instead of dictating the panel's width.
    m_statusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    m_rpt1Label = new QLabel("—");
    m_rpt2Label = new QLabel("—");
    m_urCallLabel = new QLabel("—");
    m_myCallLabel = new QLabel("—");
    m_myCall2Label = new QLabel("—");
    m_txMessageLabel = new QLabel("—");

    auto *headerBox = new QGroupBox("Last received header");
    auto *headerForm = new QFormLayout(headerBox);
    headerForm->addRow("MYCALL:", m_myCallLabel);
    headerForm->addRow("MYCALL2:", m_myCall2Label);
    headerForm->addRow("URCALL:", m_urCallLabel);
    headerForm->addRow("RPT1:", m_rpt1Label);
    headerForm->addRow("RPT2:", m_rpt2Label);
    headerForm->addRow("TX message:", m_txMessageLabel);

    // Newest first, capped in addLastHeardEntry() -- reflector/module come
    // from the connection itself (m_connectedReflectorName/Module), not
    // parsed out of the header, since a session only ever links to one
    // reflector+module and everything heard during it necessarily came
    // via that same one.
    m_lastHeardTable = new QTableWidget(0, 5);
    m_lastHeardTable->setHorizontalHeaderLabels({"Time", "Callsign", "Reflector", "Module", "Message"});
    m_lastHeardTable->verticalHeader()->setVisible(false);
    m_lastHeardTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_lastHeardTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_lastHeardTable->setFocusPolicy(Qt::NoFocus); // a passive log, not a tab stop
    m_lastHeardTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_lastHeardTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_lastHeardTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_lastHeardTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_lastHeardTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    m_lastHeardTable->setStyleSheet(theme::contentBackground("QTableWidget"));

    m_addFavouriteButton = new QPushButton("Add to favourites");
    m_addFavouriteButton->setEnabled(false);
    connect(m_addFavouriteButton, &QPushButton::clicked, this, [this] {
        addFavouriteItem(selectedReflectorName(), selectedModule());
        saveFavourites();
        updateAddFavouriteEnabled();
    });

    // Picking an entry loads its reflector and module into the combos, as
    // if chosen by hand. itemClicked rather than currentItemChanged so
    // re-clicking the already-current entry still applies it.
    m_favouritesList = new QListWidget;
    m_favouritesList->setStyleSheet(theme::contentBackground("QListWidget"));
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
    connect(m_audioLevels, &AudioLevelsPanel::autoLevelToggled, this, [this](bool on) {
        m_autoLevel.setEnabled(on);
        emit autoLevelChanged(on);
    });
    // Record what's heard; only enabled while connected (see setConnected).
    connect(m_audioLevels, &AudioLevelsPanel::recordToggled, this, [this](bool on) {
        if (!on) {
            stopRecording();
            return;
        }
        if (startRecording(QString("D-Star %1 %2").arg(m_connectedReflectorName).arg(moduleText(m_connectedModule))).isEmpty()) m_audioLevels->setRecording(false);
    });
    layout->addWidget(m_audioLevels);
    makeLargePttButton(m_pttButton);
    layout->addWidget(m_pttButton);

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
    // Fetching the DPlus list means logging in as the callsign, so not
    // before one has been saved (the default isn't the user's).
    if (QFile::exists(GuiSettings::filePath())) m_model->refreshDPlus(m_settings.callsign);
    updateConnectButtonEnabled();
}

void DStarTab::applySettings(const GuiSettings &settings) {
    m_settings = settings;
    m_model->refreshDPlus(m_settings.callsign);
}

QWidget *DStarTab::spaceExemptFocusWidget() const { return m_reflectorCombo->lineEdit(); }

// currentIndex() alone isn't enough: QComboBox doesn't automatically
// invalidate it just because the user edited the line edit's text away
// from whatever that row says (there's no "unselect on edit" built in).
// So a pick only counts if the row it points to is still what's actually
// displayed right now. Typing a name exactly ("ZL2VH", "ref001") counts
// too, without picking it from the popup -- the text is left as typed.
int DStarTab::selectedRow() const {
    int row = m_reflectorCombo->currentIndex();
    QString text = m_reflectorCombo->currentText();
    if (row >= 0 && text == m_model->data(m_model->index(row, 0), Qt::DisplayRole).toString()) return row;
    QString typed = text.trimmed();
    if (typed.isEmpty()) return -1;
    for (int r = 0; r < m_model->rowCount(); r++) {
        if (m_model->data(m_model->index(r, 0), ReflectorListModel::NameRole).toString().compare(typed, Qt::CaseInsensitive) == 0)
            return r;
    }
    return -1;
}

QString DStarTab::selectedHost() const {
    int row = selectedRow();
    if (row < 0) return {};
    return m_model->data(m_model->index(row, 0), ReflectorListModel::HostRole).toString();
}

QString DStarTab::selectedReflectorName() const {
    int row = selectedRow();
    if (row < 0) return {};
    return m_model->data(m_model->index(row, 0), ReflectorListModel::NameRole).toString();
}

bool DStarTab::selectedIsDPlus() const {
    int row = selectedRow();
    if (row < 0) return false;
    return m_model->data(m_model->index(row, 0), ReflectorListModel::IsDPlusRole).toBool();
}

char DStarTab::selectedModule() const {
    char module = static_cast<char>(m_targetModule->currentData().toInt());
    if (module == '*' && !selectedIsDPlus()) return 0;
    return module;
}

void DStarTab::updateModuleChoices() {
    // Greyed out unless a DPlus entry is picked -- and deselected when a
    // DExtra reflector is picked with it chosen, rather than left there
    // greyed with Connect disabled for no visible reason. Not while typing
    // (no valid pick yet) or mid list refresh (see m_restoringPick).
    if (auto *model = qobject_cast<QStandardItemModel *>(m_targetModule->model())) {
        if (QStandardItem *all = model->item(1)) all->setEnabled(selectedIsDPlus());
    }
    if (!m_restoringPick && selectedRow() >= 0 && !selectedIsDPlus() &&
        m_targetModule->currentData().toInt() == '*')
        m_targetModule->setCurrentIndex(0); // re-enters updateConnectButtonEnabled() once, harmlessly
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
    if (selectedModule() == 0) {
        QMessageBox::information(this, "No module selected", "Pick a target module first.");
        return;
    }
    if (m_settings.thumbdvTarget().isEmpty()) {
        QMessageBox::warning(this, "No ThumbDV device", "Set your ThumbDV (serial device or AMBEServer host) in Settings first.");
        return;
    }

    QString reflectorName = selectedReflectorName();
    char targetModule = selectedModule();
    bool isDPlus = selectedIsDPlus();
    m_connectButton->setStyleSheet("");
    m_statusLabel->setStyleSheet(theme::statusLabelStyle());
    setBusy(true, "Connecting to " + (isDPlus ? reflectorName : host) + "...");

    if (m_worker.joinable()) m_worker.join();
    m_worker = std::thread(&DStarTab::connectWorker, this, host, reflectorName, isDPlus, targetModule, m_settings);
}

void DStarTab::connectWorker(QString host, QString reflectorName, bool isDPlus, char targetModule, GuiSettings settings) {
    QString error, note;
    bool ok = true, txAllowed = true;

    m_dv = std::make_unique<SerialDV::DVController>();
    if (!m_dv->open(settings.thumbdvTarget().toStdString())) {
        error = "Failed to open ThumbDV " + settings.thumbdvTarget();
        ok = false;
    } else {
        checkThumbdvLatency(settings);
        m_vocoder = std::make_unique<audio::VocoderPipeline>(*m_dv, SerialDV::DVRate3600x2400);
        if (!m_vocoder->ok()) {
            error = "ThumbDV " + settings.thumbdvTarget() + " didn't accept D-Star's AMBE rate";
            ok = false;
        }
    }

    if (ok && !m_capture.open(settings.audioInputDevice.toStdString(), SND_PCM_STREAM_CAPTURE)) {
        error = "Failed to open audio input device " + settings.audioInputDevice;
        if (!m_capture.openError().empty()) error += ": " + QString::fromStdString(m_capture.openError());
        ok = false;
    }

    if (ok && !m_playback.open(settings.audioOutputDevice.toStdString(), SND_PCM_STREAM_PLAYBACK)) {
        error = "Failed to open audio output device " + settings.audioOutputDevice;
        if (!m_playback.openError().empty()) error += ": " + QString::fromStdString(m_playback.openError());
        ok = false;
    }

    if (ok) {
        dextra::g_running = 1;
        error = isDPlus ? connectDPlus(host, reflectorName, targetModule, settings, &note, &txAllowed)
                      : connectDExtra(host, targetModule, settings);
        ok = error.isEmpty();
    }

    if (!ok) {
        m_client.reset();
        m_capture.close();
        m_playback.close();
        m_vocoder.reset();
        if (m_dv) m_dv->close();
        m_dv.reset();
    }

    QMetaObject::invokeMethod(
        this,
        [this, ok, error, reflectorName, isDPlus, targetModule, note, txAllowed]() {
            m_connectedDPlus = isDPlus;
            onConnectFinished(ok, error, reflectorName, targetModule, note, txAllowed);
        },
        Qt::QueuedConnection);
}

QString DStarTab::connectDExtra(const QString &host, char targetModule, const GuiSettings &settings) {
    char myModule = settings.moduleSuffix.isEmpty() ? 'B' : settings.moduleSuffix.at(0).toLatin1();
    auto client = std::make_unique<dextra::DextraClient>(
        m_dv.get(), [this](const short *pcm) { m_rxQueue.push(pcm); }, nullptr);
    client->setVocoder(m_vocoder.get());
    client->setIdentity(settings.callsign.toStdString(), myModule, settings.suffix.toStdString());
    client->setTxMessage(settings.txMessage.toStdString());
    // Runs on the network thread once client->run() starts -- marshal
    // to the GUI thread rather than touching widgets directly here.
    client->setHeaderSink([this](const dextra::DStarHeader &header) {
        QMetaObject::invokeMethod(
            this, [this, header]() { onHeaderReceived(header); }, Qt::QueuedConnection);
    });
    client->setTxMessageSink([this](const dextra::DStarHeader &header, const std::string &message) {
        QMetaObject::invokeMethod(
            this, [this, header, text = QString::fromStdString(message)]() { onTxMessageReceived(header, text); },
            Qt::QueuedConnection);
    });

    if (!client->open(host.toStdString(), targetModule)) return "Failed to open network socket to " + host;
    if (!client->link()) return "Reflector did not accept the connection (NAK or timeout)";
    m_client = std::move(client);
    return {};
}

QString DStarTab::connectDPlus(QString host, const QString &reflectorName, char targetModule,
                               const GuiSettings &settings, QString *note, bool *txAllowed) {
    // Log in to the DPlus authentication server first -- reflectors and
    // gateways only accept callsigns it has vouched for. Its reply is the
    // current list, so the address comes from there when it's listed.
    std::vector<dplus::GatewayEntry> list;
    std::string authError;
    if (!dplus::authenticate(settings.callsign.toStdString(), &list, authError))
        return "Couldn't log in to the DPlus authentication server: " + QString::fromStdString(authError);
    for (const auto &g : list) {
        if (QString::fromStdString(g.name) == reflectorName) host = QString::fromStdString(g.address);
    }
    QMetaObject::invokeMethod(
        m_model, [model = m_model, list]() mutable { model->setDPlusList(std::move(list)); }, Qt::QueuedConnection);

    char suffix = settings.moduleSuffix.isEmpty() ? ' ' : settings.moduleSuffix.at(0).toLatin1();
    QString login = settings.callsign + (suffix == ' ' ? QString() : QString(" ") + QChar(suffix));
    auto client = std::make_unique<dplus::DPlusClient>();
    client->setIdentity(settings.callsign.toStdString(), suffix, settings.suffix.toStdString());
    client->setTxMessage(settings.txMessage.toStdString());
    client->setTarget(reflectorName.toStdString(), targetModule);
    client->setLiveRx(m_vocoder.get(), [this](const short *pcm) { m_rxQueue.push(pcm); });
    // Every module's streams' headers, not just the one played -- see
    // dplus_client.h. Marshalled to the GUI thread, as for DExtra.
    client->setHeaderSink([this](const dextra::DStarHeader &header) {
        QMetaObject::invokeMethod(
            this, [this, header]() { onHeaderReceived(header); }, Qt::QueuedConnection);
    });
    client->setTxMessageSink([this](const dextra::DStarHeader &header, const std::string &message) {
        QMetaObject::invokeMethod(
            this, [this, header, text = QString::fromStdString(message)]() { onTxMessageReceived(header, text); },
            Qt::QueuedConnection);
    });

    if (!client->open(host.toStdString())) return "Failed to open network socket to " + host;
    switch (client->link()) {
    case dplus::LinkResult::Accepted:
        break;
    case dplus::LinkResult::ReadOnly:
        *note = QString("listen only: %1 isn't one of %2's registered D-Star terminals (module suffix in Settings)")
                    .arg(login, settings.callsign);
        *txAllowed = false;
        break;
    case dplus::LinkResult::Refused:
        if (client->loginReply() == "BUSY")
            return QString("%1 says %2 is already connected. Each name gets one login: turn off whatever else is "
                           "linked as %2 (a radio's Repeater Monitor or Terminal Mode, a hotspot), or pick a "
                           "different module suffix in Settings.")
                .arg(reflectorName, login);
        return QString("%1 refused the login as %2 (%3). Is %4 registered for D-Star?")
            .arg(reflectorName, login, QString::fromStdString(client->loginReply()), settings.callsign);
    case dplus::LinkResult::NoAnswer:
        return QString("No answer from %1 (%2)").arg(reflectorName, host);
    }
    if (targetModule == '*') *txAllowed = false;
    m_client = std::move(client);
    return {};
}

void DStarTab::onConnectFinished(bool ok, QString error, QString reflectorName, char targetModule, QString note,
                                 bool txAllowed) {
    if (!ok) {
        setBusy(false, "Connect failed: " + error);
        m_connectButton->setStyleSheet(kErrorButtonStyle);
        m_statusLabel->setStyleSheet(theme::errorLabelStyle());
        QMessageBox::warning(this, "Connect failed", error);
        return;
    }

    m_rpt1Label->setText("—");
    m_rpt2Label->setText("—");
    m_urCallLabel->setText("—");
    m_myCallLabel->setText("—");
    m_myCall2Label->setText("—");
    m_txMessageLabel->setText("—");

    m_connectedReflectorName = reflectorName.isEmpty() ? "?" : reflectorName;
    m_connectedModule = targetModule;
    m_txAllowed = txAllowed;

    m_pttActive.store(false);
    m_captureThread = std::thread(dextra::captureThread, m_vocoder.get(), &m_capture, m_client.get(),
                                   [this] { return m_pttActive.load(); });
    m_playbackThread = std::thread(audio::playbackThread, &m_playback, &m_rxQueue, &dextra::g_running, &m_recorder,
                                   &m_autoLevel);
    // run() returning by itself means the link is gone (a DPlus server
    // that went silent); disconnecting the rest of the session is then
    // the GUI thread's job.
    m_networkThread = std::thread([this] {
        m_client->run();
        if (!dextra::g_running) return;
        QMetaObject::invokeMethod(
            this,
            [this] {
                if (!m_connected || m_busy) return;
                m_lostConnection = true;
                startDisconnect();
            },
            Qt::QueuedConnection);
    });

    QString status = targetModule == '*'
                         ? QString("Connected to %1, all modules (listen only)").arg(m_connectedReflectorName)
                         : QString("Connected to %1, module %2").arg(m_connectedReflectorName).arg(QChar(targetModule));
    if (!note.isEmpty()) status += " \u2014 " + note;
    setConnected(true, status);
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
    m_txMessageLabel->setText("—");
    if (m_lostConnection) {
        m_lostConnection = false;
        setConnected(false, "Connection to " + m_connectedReflectorName + " lost.");
        m_connectButton->setStyleSheet(kErrorButtonStyle);
        m_statusLabel->setStyleSheet(theme::errorLabelStyle());
        return;
    }
    setConnected(false, "Disconnected.");
}

void DStarTab::onHeaderReceived(dextra::DStarHeader header) {
    m_rpt1Label->setText(QString::fromStdString(header.rpt1));
    m_rpt2Label->setText(QString::fromStdString(header.rpt2));
    m_urCallLabel->setText(QString::fromStdString(header.urCall));
    m_myCallLabel->setText(QString::fromStdString(header.myCall));
    m_myCall2Label->setText(QString::fromStdString(header.myCall2));
    m_txMessageLabel->setText("—");
    addLastHeardEntry(header);
}

namespace {
QString lastHeardCallsign(const dextra::DStarHeader &header) {
    QString callsign = QString::fromStdString(header.myCall);
    if (!header.myCall2.empty()) callsign += "/" + QString::fromStdString(header.myCall2);
    return callsign;
}
} // namespace

void DStarTab::onTxMessageReceived(dextra::DStarHeader header, QString message) {
    QString callsign = lastHeardCallsign(header);
    // The header box shows the latest header, which may be another
    // stream's by now (a DPlus session sees every module's).
    if (m_myCallLabel->text() == QString::fromStdString(header.myCall) &&
        m_rpt1Label->text() == QString::fromStdString(header.rpt1))
        m_txMessageLabel->setText(message);
    // The newest Last Heard row for this callsign: its row was added when
    // the header came, moments before, so it's near the top.
    for (int row = 0; row < m_lastHeardTable->rowCount() && row < 10; row++) {
        QTableWidgetItem *item = m_lastHeardTable->item(row, 1);
        if (item && item->text() == callsign) {
            m_lastHeardTable->setItem(row, 4, new QTableWidgetItem(message));
            break;
        }
    }
}

void DStarTab::addLastHeardEntry(const dextra::DStarHeader &header) {
    QString callsign = lastHeardCallsign(header);

    m_lastHeardTable->insertRow(0);
    m_lastHeardTable->setItem(0, 0, new QTableWidgetItem(QTime::currentTime().toString("HH:mm:ss")));
    m_lastHeardTable->setItem(0, 1, new QTableWidgetItem(callsign));
    m_lastHeardTable->setItem(0, 2, new QTableWidgetItem(m_connectedReflectorName));
    // A DPlus session gets every module's streams; each says which it's on.
    char module = m_connectedDPlus ? dplus::streamModule(header) : m_connectedModule;
    m_lastHeardTable->setItem(0, 3, new QTableWidgetItem(module == ' ' ? QString("?") : QString(QChar(module))));

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
    // Waits for anything still in flight; after the threads above, since
    // they submit to it.
    m_vocoder.reset();
    if (m_dv) m_dv->close();
    dextra::g_running = 1;
}

// Applied to the AlsaPcm objects themselves, which outlive any one
// connection -- so this works while disconnected too and carries over to
// the next connect.
void DStarTab::setAutoLevel(bool on) {
    m_audioLevels->setAutoLevel(on);
    m_autoLevel.setEnabled(on);
}

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
    if (connected) resetThumbdvTiming();
    else clearVocoderShared();
    m_audioLevels->setRecordEnabled(connected);
    if (!connected) {
        // The playback thread has stopped by now, so the file is complete.
        stopRecording();
        m_audioLevels->setRecording(false);
    }
    m_connectButton->setText(connected ? "Disconnect" : "Connect");
    m_connectButton->setStyleSheet(connected ? kConnectedButtonStyle : "");
    m_statusLabel->setStyleSheet(theme::statusLabelStyle());
    m_pttButton->setEnabled(connected && m_txAllowed);
    if (!connected) m_pttButton->setChecked(false); // in case we disconnected mid-send
    setBusy(false, status);
}

void DStarTab::updateVocoderStatus() {
    // m_dv is only created and destroyed around connect/disconnect, and
    // m_connected is true (with m_busy false) only in between, both set
    // on this thread -- so while that holds it's safe to read here.
    if (!m_connected || m_busy || !m_dv) return;
    checkThumbdvTiming(*m_dv, m_settings.thumbdvIsNetwork());
    checkVocoderShared(m_vocoder.get(), m_settings.thumbdvIsNetwork());
    bool down = !m_dv->isResponding();
    if (down == m_vocoderDown) return;
    m_vocoderDown = down;
    m_statusLabel->setText(down ? m_connectedStatus + " — ThumbDV not responding" : m_connectedStatus);
    m_statusLabel->setStyleSheet(down ? theme::errorLabelStyle() : theme::statusLabelStyle());
}

void DStarTab::updateConnectButtonEnabled() {
    // Once connected, the button becomes Disconnect and should always stay
    // usable regardless of what's selected in the (by then disabled)
    // reflector list and module combo.
    updateModuleChoices();
    bool readyToConnect = !selectedHost().isEmpty() && selectedModule() != 0;
    m_connectButton->setEnabled(!m_busy && (m_connected || readyToConnect));
    updateAddFavouriteEnabled();
}

void DStarTab::updateAddFavouriteEnabled() {
    if (!m_addFavouriteButton) return; // a selection signal fired mid-construction
    bool ready = !selectedHost().isEmpty() && selectedModule() != 0;
    m_addFavouriteButton->setEnabled(ready && !isFavourite(selectedReflectorName(), selectedModule()));
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
    if (reflectorName.isEmpty() || ((module < 'A' || module > 'Z') && module != '*') || isFavourite(reflectorName, module))
        return;
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
            m_targetModule->setCurrentIndex(qMax(0, m_targetModule->findData(static_cast<int>(module))));
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
