#include "ysftab.h"

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

#include <cstring>

#include "audio_gain.h"
#include "audiolevelspanel.h"
#include "theme.h"
#include "ysf_audio.h"
#include "ysfreflectormodel.h"

namespace {
const char *kConnectedButtonStyle = "background-color: #4CAF50; color: white;";
const char *kErrorButtonStyle = "background-color: #f44336; color: white;";
const char *kSendingButtonStyle = "background-color: #ff9800; color: white;";

QString dgIdText(int dgId) { return QString("%1").arg(dgId, 2, 10, QChar('0')); }

QString dgIdsFilePath() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + "/ysf_dgids.json";
}

QString favouritesFilePath() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + "/ysf_favourites.json";
}

// How Last Heard shows a stream's data type; only DN mode is played.
QString modeText(int dataType) {
    switch (dataType) {
    case 2: return "DN";
    case 3: return "VW (not played)";
    case 0: return "V/D 1 (not played)";
    case 1: return "data";
    default: return "?";
    }
}
} // namespace

YsfTab::YsfTab(const GuiSettings &settings, QWidget *parent) : ProtocolTab(parent), m_settings(settings) {
    m_model = new YsfReflectorModel(this);
    connect(m_model, &YsfReflectorModel::refreshFailed, this,
            [this](const QString &error) { m_statusLabel->setText("Reflector list: " + error); });

    // Editable combo box with a completer, as on the D-Star tab: typing
    // narrows a popup of matches.
    m_reflectorCombo = new QComboBox;
    m_reflectorCombo->setEditable(true);
    m_reflectorCombo->setInsertPolicy(QComboBox::NoInsert);
    m_reflectorCombo->setModel(m_model);
    m_reflectorCombo->setCurrentIndex(-1);
    m_reflectorCombo->lineEdit()->setPlaceholderText("Search YSF reflectors (e.g. America, 03073, Parrot)...");
    m_reflectorCombo->setStyleSheet(theme::contentBackground("QComboBox QAbstractItemView"));

    // A list refresh resets the combo box: keep what was picked.
    connect(m_model, &QAbstractItemModel::modelAboutToBeReset, this, [this] {
        int row = selectedRow();
        m_pickBeforeReset =
            row >= 0 ? m_model->data(m_model->index(row, 0), YsfReflectorModel::NameRole).toString() : QString();
    });
    connect(m_model, &QAbstractItemModel::modelReset, this, [this] {
        m_reflectorCombo->setCurrentIndex(-1);
        for (int row = 0; !m_pickBeforeReset.isEmpty() && row < m_model->rowCount(); row++) {
            if (m_model->data(m_model->index(row, 0), YsfReflectorModel::NameRole).toString() == m_pickBeforeReset) {
                m_reflectorCombo->setCurrentIndex(row);
                break;
            }
        }
        m_pickBeforeReset.clear();
        updateConnectButtonEnabled();
    });

    auto *completer = new QCompleter(m_model, this);
    completer->setCompletionRole(Qt::DisplayRole);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    completer->popup()->setStyleSheet(theme::contentBackground("QAbstractItemView"));
    m_reflectorCombo->setCompleter(completer);
    connect(m_reflectorCombo->lineEdit(), &QLineEdit::textChanged, this, &YsfTab::updateConnectButtonEnabled);
    connect(m_reflectorCombo, &QComboBox::currentIndexChanged, this, &YsfTab::updateConnectButtonEnabled);

    // The DG-ID to transmit with. On reflectors with rooms (YCS, PA7LIM's
    // JAPANLINK) it picks the room when you key up -- including 00, which
    // JAPANLINK takes as its welcome room; connecting doesn't change room
    // (JAPANLINK puts you back in your last one). So it isn't stored with
    // favourites. Changeable while connected.
    m_dgIdBox = new QComboBox;
    for (int i = 0; i <= 99; i++) m_dgIdBox->addItem(dgIdText(i));
    m_dgIdBox->setToolTip("DG-ID for your transmissions (00-99). On reflectors with rooms, such as YCS servers\n"
                          "and JAPANLINK, it picks the room: your next transmission moves you to it.");
    {
        QFile f(dgIdsFilePath());
        if (f.open(QIODevice::ReadOnly)) m_dgIds = QJsonDocument::fromJson(f.readAll()).object();
    }
    connect(m_dgIdBox, &QComboBox::currentIndexChanged, this, [this](int dgId) {
        if (m_client) m_client->setDgId(dgId);
        if (m_dgIdReflector.isEmpty() || m_dgIds.value(m_dgIdReflector).toInt(-1) == dgId) return;
        m_dgIds[m_dgIdReflector] = dgId;
        QFile f(dgIdsFilePath());
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QJsonDocument(m_dgIds).toJson(QJsonDocument::Indented));
    });

    m_connectButton = new QPushButton("Connect");
    connect(m_connectButton, &QPushButton::clicked, this, [this] {
        if (m_busy) return;
        if (m_connected) startDisconnect();
        else startConnect();
    });

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
    m_statusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    m_sourceLabel = new QLabel("—");
    m_destinationLabel = new QLabel("—");
    m_gatewayLabel = new QLabel("—");
    m_modeLabel = new QLabel("—");
    m_dgIdLabel = new QLabel("—");
    auto *headerBox = new QGroupBox("Last received");
    auto *headerForm = new QFormLayout(headerBox);
    headerForm->addRow("Callsign:", m_sourceLabel);
    headerForm->addRow("To:", m_destinationLabel);
    headerForm->addRow("Via:", m_gatewayLabel);
    headerForm->addRow("Mode:", m_modeLabel);
    headerForm->addRow("DG-ID:", m_dgIdLabel);

    m_lastHeardTable = new QTableWidget(0, 5);
    m_lastHeardTable->setHorizontalHeaderLabels({"Time", "Callsign", "Via", "DG-ID", "Mode"});
    m_lastHeardTable->verticalHeader()->setVisible(false);
    m_lastHeardTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_lastHeardTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_lastHeardTable->setFocusPolicy(Qt::NoFocus);
    m_lastHeardTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_lastHeardTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_lastHeardTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_lastHeardTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_lastHeardTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_lastHeardTable->setStyleSheet(theme::contentBackground("QTableWidget"));

    m_addFavouriteButton = new QPushButton("Add to favourites");
    m_addFavouriteButton->setEnabled(false);
    connect(m_addFavouriteButton, &QPushButton::clicked, this, [this] {
        int row = selectedRow();
        if (row < 0) return;
        addFavouriteItem(m_model->data(m_model->index(row, 0), YsfReflectorModel::NameRole).toString());
        saveFavourites();
        updateAddFavouriteEnabled();
    });
    m_favouritesList = new QListWidget;
    m_favouritesList->setStyleSheet(theme::contentBackground("QListWidget"));
    auto applyFavourite = [this](QListWidgetItem *item) { selectFavourite(item->text()); };
    connect(m_favouritesList, &QListWidget::itemClicked, this, applyFavourite);
    connect(m_favouritesList, &QListWidget::itemActivated, this, applyFavourite);
    auto removeFavourite = [this](QListWidgetItem *item) {
        delete m_favouritesList->takeItem(m_favouritesList->row(item));
        saveFavourites();
        updateAddFavouriteEnabled();
    };
    m_favouritesList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_favouritesList, &QListWidget::customContextMenuRequested, this, [this, removeFavourite](const QPoint &pos) {
        QListWidgetItem *item = m_favouritesList->itemAt(pos);
        if (!item) return;
        QMenu menu;
        QAction *remove = menu.addAction("Remove from favourites");
        if (menu.exec(m_favouritesList->viewport()->mapToGlobal(pos)) == remove) removeFavourite(item);
    });
    auto *deleteShortcut = new QShortcut(QKeySequence::Delete, m_favouritesList);
    deleteShortcut->setContext(Qt::WidgetShortcut);
    connect(deleteShortcut, &QShortcut::activated, this, [this, removeFavourite] {
        if (QListWidgetItem *item = m_favouritesList->currentItem()) removeFavourite(item);
    });

    auto *favouriteRow = new QHBoxLayout;
    favouriteRow->addWidget(new QLabel("DG-ID:"));
    favouriteRow->addWidget(m_dgIdBox);
    favouriteRow->addStretch();
    favouriteRow->addWidget(m_addFavouriteButton);

    auto *bottomRow = new QHBoxLayout;
    bottomRow->addWidget(m_connectButton);
    bottomRow->addWidget(m_statusLabel, 1);

    auto *layout = new QVBoxLayout;
    layout->addWidget(m_reflectorCombo);
    layout->addLayout(favouriteRow);
    layout->addLayout(bottomRow);
    layout->addWidget(new QLabel("Favourites:"));
    layout->addWidget(m_favouritesList, 1);
    layout->addWidget(headerBox);

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
    connect(m_audioLevels, &AudioLevelsPanel::recordToggled, this, [this](bool on) {
        if (!on) {
            stopRecording();
            return;
        }
        if (startRecording("YSF " + m_connectedName).isEmpty()) m_audioLevels->setRecording(false);
    });
    layout->addWidget(m_audioLevels);
    makeLargePttButton(m_pttButton);
    layout->addWidget(m_pttButton);

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

QWidget *YsfTab::spaceExemptFocusWidget() const { return m_reflectorCombo->lineEdit(); }

int YsfTab::selectedRow() const {
    int row = m_reflectorCombo->currentIndex();
    QString text = m_reflectorCombo->currentText();
    if (row >= 0 && text == m_model->data(m_model->index(row, 0), Qt::DisplayRole).toString()) return row;
    QString typed = text.trimmed();
    if (typed.isEmpty()) return -1;
    for (int r = 0; r < m_model->rowCount(); r++) {
        QModelIndex index = m_model->index(r, 0);
        QString display = m_model->data(index, Qt::DisplayRole).toString();
        if (m_model->data(index, YsfReflectorModel::NameRole).toString().compare(typed, Qt::CaseInsensitive) == 0 ||
            display.endsWith("(" + typed + ")"))
            return r;
    }
    return -1;
}

void YsfTab::rememberedDgIdFor(const QString &reflector) {
    if (!m_dgIdBox || reflector == m_dgIdReflector) return;
    m_dgIdReflector = reflector;
    if (!reflector.isEmpty()) m_dgIdBox->setCurrentIndex(m_dgIds.value(reflector).toInt(0));
}

void YsfTab::updateConnectButtonEnabled() {
    // The box follows the picked reflector -- not while connected, when it
    // belongs to the connected one whatever the picker shows.
    int row = selectedRow();
    if (!m_connected && !m_busy)
        rememberedDgIdFor(row >= 0 ? m_model->data(m_model->index(row, 0), YsfReflectorModel::NameRole).toString()
                                   : QString());
    if (!m_connectButton) return; // a signal during construction
    m_connectButton->setEnabled(!m_busy && (m_connected || row >= 0));
    updateAddFavouriteEnabled();
}

void YsfTab::startConnect() {
    int row = selectedRow();
    if (row < 0) {
        QMessageBox::information(this, "No reflector selected", "Pick a YSF reflector from the list first.");
        return;
    }
    if (m_settings.thumbdvTarget().isEmpty()) {
        QMessageBox::warning(this, "No ThumbDV device", "Set your ThumbDV (serial device or AMBEServer host) in Settings first.");
        return;
    }
    QModelIndex index = m_model->index(row, 0);
    QString name = m_model->data(index, YsfReflectorModel::NameRole).toString();
    QString host = m_model->data(index, YsfReflectorModel::HostRole).toString();
    int port = m_model->data(index, YsfReflectorModel::PortRole).toInt();
    m_connectButton->setStyleSheet("");
    m_statusLabel->setStyleSheet(theme::statusLabelStyle());
    setBusy(true, "Connecting to " + name + "...");
    if (m_worker.joinable()) m_worker.join();
    m_worker = std::thread(&YsfTab::connectWorker, this, name, host, port, m_settings);
}

void YsfTab::connectWorker(QString name, QString host, int port, GuiSettings settings) {
    QString error;
    bool ok = true;

    m_dv = std::make_unique<SerialDV::DVController>();
    if (!m_dv->open(settings.thumbdvTarget().toStdString())) {
        error = "Failed to open ThumbDV " + settings.thumbdvTarget();
        ok = false;
    } else {
        checkThumbdvLatency(settings);
        // YSF DN mode's AMBE+2 is at DMR's rate.
        m_vocoder = std::make_unique<audio::VocoderPipeline>(*m_dv, SerialDV::DVRate3600x2450);
        if (!m_vocoder->ok()) {
            error = "ThumbDV " + settings.thumbdvTarget() + " didn't accept YSF's AMBE rate";
            ok = false;
        } else {
            // A frame of silence, to fill out the last YSF frame of each
            // transmission.
            static const short silentPcm[SerialDV::MBE_AUDIO_BLOCK_SIZE] = {};
            m_vocoder->encode(silentPcm, [this](const uint8_t *ambe) { std::memcpy(m_silence, ambe, sizeof(m_silence)); });
            m_vocoder->drain();
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
        m_client = std::make_unique<ysf::YsfClient>();
        m_client->setDgId(m_dgIdBox->currentIndex());
        // On the network thread once run() starts: decoded through the
        // vocoder; stream starts marshalled to the GUI thread.
        m_client->setVoiceSink([this](const uint8_t *ambe) {
            m_vocoder->decode(ambe, [this](const short *pcm) { m_rxQueue.push(pcm); });
        });
        m_client->setStreamStartSink([this](const ysf::StreamInfo &info) {
            QMetaObject::invokeMethod(this, [this, info] { onStreamStarted(info); }, Qt::QueuedConnection);
        });
        ysf::g_running = 1;
        if (!m_client->open(host.toStdString(), port)) {
            error = "Failed to open network socket to " + host;
            ok = false;
        } else if (!m_client->link(settings.callsign.toStdString())) {
            error = "No answer from " + name + " (" + host + ":" + QString::number(port) + ")";
            ok = false;
        }
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
        this, [this, ok, error, name] { onConnectFinished(ok, error, name); }, Qt::QueuedConnection);
}

void YsfTab::onConnectFinished(bool ok, QString error, QString name) {
    if (!ok) {
        setBusy(false, "Connect failed: " + error);
        m_connectButton->setStyleSheet(kErrorButtonStyle);
        m_statusLabel->setStyleSheet(theme::errorLabelStyle());
        QMessageBox::warning(this, "Connect failed", error);
        return;
    }
    for (QLabel *l : {m_sourceLabel, m_destinationLabel, m_gatewayLabel, m_modeLabel, m_dgIdLabel}) l->setText("—");
    m_connectedName = name;
    m_pttActive.store(false);
    m_captureThread = std::thread(ysf::captureThread, m_vocoder.get(), &m_capture, m_client.get(), m_silence,
                                  [this] { return m_pttActive.load(); });
    m_playbackThread =
        std::thread(audio::playbackThread, &m_playback, &m_rxQueue, &ysf::g_running, &m_recorder, &m_autoLevel);
    // run() returning by itself means the reflector went silent.
    m_networkThread = std::thread([this] {
        m_client->run(&ysf::g_running);
        if (!ysf::g_running) return;
        QMetaObject::invokeMethod(
            this,
            [this] {
                if (!m_connected || m_busy) return;
                m_lostConnection = true;
                startDisconnect();
            },
            Qt::QueuedConnection);
    });
    setConnected(true, "Connected to " + name);
}

void YsfTab::onStreamStarted(ysf::StreamInfo info) {
    QString source = QString::fromStdString(info.source);
    m_sourceLabel->setText(source);
    m_destinationLabel->setText(QString::fromStdString(info.destination));
    m_gatewayLabel->setText(QString::fromStdString(info.gateway));
    m_modeLabel->setText(modeText(info.dataType));
    m_dgIdLabel->setText(info.dgId < 0 ? "?" : dgIdText(info.dgId));

    m_lastHeardTable->insertRow(0);
    m_lastHeardTable->setItem(0, 0, new QTableWidgetItem(QTime::currentTime().toString("HH:mm:ss")));
    m_lastHeardTable->setItem(0, 1, new QTableWidgetItem(source));
    m_lastHeardTable->setItem(0, 2, new QTableWidgetItem(QString::fromStdString(info.gateway)));
    m_lastHeardTable->setItem(0, 3, new QTableWidgetItem(info.dgId < 0 ? "?" : dgIdText(info.dgId)));
    m_lastHeardTable->setItem(0, 4, new QTableWidgetItem(modeText(info.dataType)));
    while (m_lastHeardTable->rowCount() > 100) m_lastHeardTable->removeRow(m_lastHeardTable->rowCount() - 1);
}

void YsfTab::startDisconnect() {
    setBusy(true, "Disconnecting...");
    if (m_worker.joinable()) m_worker.join();
    m_worker = std::thread([this] {
        stopSessionBlocking();
        QMetaObject::invokeMethod(this, [this] { onDisconnectFinished(); }, Qt::QueuedConnection);
    });
}

void YsfTab::onDisconnectFinished() {
    m_client.reset();
    m_dv.reset();
    if (m_lostConnection) {
        m_lostConnection = false;
        setConnected(false, "Connection to " + m_connectedName + " lost.");
        m_connectButton->setStyleSheet(kErrorButtonStyle);
        m_statusLabel->setStyleSheet(theme::errorLabelStyle());
        return;
    }
    setConnected(false, "Disconnected.");
}

void YsfTab::stopSessionBlocking() {
    ysf::g_running = 0;
    m_pttActive.store(false);
    if (m_captureThread.joinable()) m_captureThread.join();
    if (m_playbackThread.joinable()) m_playbackThread.join();
    if (m_networkThread.joinable()) m_networkThread.join();
    if (m_client) m_client->disconnect();
    m_capture.close();
    m_playback.close();
    m_vocoder.reset(); // after the threads that submit to it
    if (m_dv) m_dv->close();
    ysf::g_running = 1;
}

void YsfTab::setAutoLevel(bool on) {
    m_audioLevels->setAutoLevel(on);
    m_autoLevel.setEnabled(on);
}

void YsfTab::setVolumes(int mic, int speaker) {
    m_audioLevels->setVolumes(mic, speaker);
    m_capture.setGain(sliderToGain(mic));
    m_playback.setGain(sliderToGain(speaker));
}

void YsfTab::setBusy(bool busy, const QString &status) {
    m_busy = busy;
    m_reflectorCombo->setEnabled(!busy && !m_connected);
    m_statusLabel->setText(status);
    updateConnectButtonEnabled();
    emit stateChanged();
}

void YsfTab::setConnected(bool connected, const QString &status) {
    m_connected = connected;
    m_connectedStatus = connected ? status : QString();
    m_vocoderDown = false;
    if (connected) resetThumbdvTiming();
    else clearVocoderShared();
    m_audioLevels->setRecordEnabled(connected);
    if (!connected) {
        stopRecording();
        m_audioLevels->setRecording(false);
    }
    m_connectButton->setText(connected ? "Disconnect" : "Connect");
    m_connectButton->setStyleSheet(connected ? kConnectedButtonStyle : "");
    m_statusLabel->setStyleSheet(theme::statusLabelStyle());
    m_pttButton->setEnabled(connected);
    if (!connected) m_pttButton->setChecked(false);
    setBusy(false, status);
}

void YsfTab::updateVocoderStatus() {
    if (!m_connected || m_busy || !m_dv) return;
    checkThumbdvTiming(*m_dv, m_settings.thumbdvIsNetwork());
    checkVocoderShared(m_vocoder.get(), m_settings.thumbdvIsNetwork());
    bool down = !m_dv->isResponding();
    if (down == m_vocoderDown) return;
    m_vocoderDown = down;
    m_statusLabel->setText(down ? m_connectedStatus + " — ThumbDV not responding" : m_connectedStatus);
    m_statusLabel->setStyleSheet(down ? theme::errorLabelStyle() : theme::statusLabelStyle());
}

bool YsfTab::isFavourite(const QString &name) const {
    for (int i = 0; i < m_favouritesList->count(); i++)
        if (m_favouritesList->item(i)->text() == name) return true;
    return false;
}

void YsfTab::addFavouriteItem(const QString &name) {
    if (name.isEmpty() || isFavourite(name)) return;
    m_favouritesList->addItem(name);
}

void YsfTab::updateAddFavouriteEnabled() {
    if (!m_addFavouriteButton) return;
    int row = selectedRow();
    m_addFavouriteButton->setEnabled(
        row >= 0 && !isFavourite(m_model->data(m_model->index(row, 0), YsfReflectorModel::NameRole).toString()));
}

void YsfTab::selectFavourite(const QString &name) {
    if (!m_reflectorCombo->isEnabled()) return; // not while connected
    for (int row = 0; row < m_model->rowCount(); row++) {
        if (m_model->data(m_model->index(row, 0), YsfReflectorModel::NameRole).toString() == name) {
            m_reflectorCombo->setCurrentIndex(row);
            return;
        }
    }
    m_statusLabel->setText(name + " isn't in the reflector list");
}

void YsfTab::loadFavourites() {
    QFile f(favouritesFilePath());
    if (!f.open(QIODevice::ReadOnly)) return;
    for (const QJsonValue &v : QJsonDocument::fromJson(f.readAll()).array()) {
        // Names; a test build briefly saved {reflector, dgid} objects.
        addFavouriteItem(v.isString() ? v.toString() : v.toObject()["reflector"].toString());
    }
}

void YsfTab::saveFavourites() const {
    QJsonArray array;
    for (int i = 0; i < m_favouritesList->count(); i++) array.append(m_favouritesList->item(i)->text());
    QFile f(favouritesFilePath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
}
