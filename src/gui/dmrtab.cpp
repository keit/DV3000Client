#include "dmrtab.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QTableWidget>
#include <QTime>
#include <QVBoxLayout>

#include <cstdio>
#include <thread>

#include "dmriddirectory.h"
#include "talkgrouplistmodel.h"

namespace {
// Same palette as DStarTab -- kept as a separate copy rather than shared
// constants, since the two tabs are otherwise independent and this is the
// only thing they'd share.
const char *kConnectedButtonStyle = "background-color: #4CAF50; color: white;";
const char *kErrorButtonStyle = "background-color: #f44336; color: white;";
const char *kErrorLabelStyle = "color: #f44336;";
const char *kSendingButtonStyle = "background-color: #ff9800; color: white;";
const char *kLightGrayBackground = "#e8e8e8";
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
    });

    m_statusLabel = new QLabel("Disconnected.");

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

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(tgRow);
    layout->addLayout(bottomRow);
    layout->addWidget(m_lastHeardTable, 1);

    // One-shot background fetch of the DMR ID directory (see
    // dmriddirectory.h) -- not refreshed periodically the way Pi-Star
    // does (every 24h), since this client isn't typically left running
    // that long, but nothing stops a future connect from re-resolving
    // against whatever's cached by then anyway.
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

uint32_t DmrTab::currentTalkgroupId() const {
    QString text = m_talkgroupCombo->currentText();
    int i = 0;
    while (i < text.size() && !text.at(i).isDigit()) i++;
    int start = i;
    while (i < text.size() && text.at(i).isDigit()) i++;
    if (i == start) return 0;
    return text.mid(start, i - start).toUInt();
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
    if (m_settings.dmrServer.isEmpty()) {
        QMessageBox::information(this, "No DMR server", "Set a DMR server (host:port) in Settings first.");
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
    m_statusLabel->setStyleSheet("");
    setBusy(true, "Connecting to " + m_settings.dmrServer + "...");

    if (m_worker.joinable()) m_worker.join();
    m_worker = std::thread(&DmrTab::connectWorker, this, m_settings);
}

void DmrTab::connectWorker(GuiSettings settings) {
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

    QString host = settings.dmrServer;
    uint16_t port = dmr::DEFAULT_PORT;
    int colonIndex = host.lastIndexOf(':');
    if (colonIndex >= 0) {
        port = static_cast<uint16_t>(host.mid(colonIndex + 1).toUInt());
        host = host.left(colonIndex);
    }

    if (ok) {
        m_client = std::make_unique<dmr::DmrClient>();
        if (!m_client->open(host.toStdString(), port)) {
            error = "Failed to open network socket to " + settings.dmrServer;
            ok = false;
        }
    }

    if (ok) {
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
        m_client->setIdentity(settings.dmrId, settings.dmrPassword.toStdString(), config);

        m_client->setVoiceRxSink(dmr::makeVoiceRxHandler(m_dv.get(), &m_rxQueue));
        // Runs on the network thread once client->run() starts -- marshal
        // to the GUI thread rather than touching widgets directly here.
        m_client->setHeaderSink([this](uint32_t srcId, uint32_t dstId) {
            QMetaObject::invokeMethod(
                this, [this, srcId, dstId]() { onHeaderReceived(srcId, dstId); }, Qt::QueuedConnection);
        });

        dmr::LinkResult result = m_client->link();
        if (result != dmr::LinkResult::Success) {
            error = QString("Master rejected the connection: %1").arg(dmr::ToString(result));
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

    setConnected(true, "Connected to " + m_settings.dmrServer);
}

void DmrTab::startDisconnect() {
    setBusy(true, "Disconnecting...");
    if (m_worker.joinable()) m_worker.join();
    m_worker = std::thread(&DmrTab::disconnectWorker, this);
}

void DmrTab::disconnectWorker() {
    stopSessionBlocking();
    QMetaObject::invokeMethod(this, [this]() { onDisconnectFinished(); }, Qt::QueuedConnection);
}

void DmrTab::onDisconnectFinished() {
    m_client.reset();
    m_dv.reset();
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
    if (m_client) m_client->disconnect();
    m_capture.close();
    m_playback.close();
    if (m_dv) m_dv->close();
    dmr::g_running = 1;
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
    m_statusLabel->setStyleSheet("");
    if (!connected) m_pttButton->setChecked(false); // in case we disconnected mid-send
    updatePttButtonEnabled();
    setBusy(false, status);
}

void DmrTab::updatePttButtonEnabled() { m_pttButton->setEnabled(m_connected && currentTalkgroupId() != 0); }
