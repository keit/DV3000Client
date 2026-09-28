#include "settingsdialog.h"

#include <alsa/asoundlib.h>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleValidator>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QStackedWidget>
#include <QPointer>
#include <QApplication>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>

#include "dextra_audio.h"
#include "dvcontroller.h"
#include "localcache.h"

namespace {

struct AlsaDeviceInfo {
    QString name;
    QString desc;
};

// Enumerates real sound cards/devices the way `aplay -l`/`arecord -l` do
// (snd_card_next + the control-interface PCM info, not the -L hint list --
// that also pulls in every software plugin: pulse, jack, oss, samplerate
// converters, etc., which is noise for picking a physical mic/speaker).
// device name is built as plughw:<card>,<device>, matching the convention
// already used in HowToStart.md's example invocations.
QList<AlsaDeviceInfo> listAlsaHardwareDevices(snd_pcm_stream_t stream) {
    QList<AlsaDeviceInfo> out;
    out << AlsaDeviceInfo{"default", "ALSA's own default routing (PulseAudio/PipeWire if running)"};
    out << AlsaDeviceInfo{"null", "Discard all samples (playback) or generate zero samples (capture)"};

    int card = -1;
    while (snd_card_next(&card) >= 0 && card >= 0) {
        snd_ctl_t *ctl = nullptr;
        if (snd_ctl_open(&ctl, QString("hw:%1").arg(card).toUtf8().constData(), 0) < 0) continue;

        snd_ctl_card_info_t *cardInfo;
        snd_ctl_card_info_alloca(&cardInfo);
        QString cardId = QString::number(card);
        QString cardName;
        if (snd_ctl_card_info(ctl, cardInfo) >= 0) {
            cardId = QString::fromUtf8(snd_ctl_card_info_get_id(cardInfo));
            cardName = QString::fromUtf8(snd_ctl_card_info_get_name(cardInfo));
        }

        int dev = -1;
        while (snd_ctl_pcm_next_device(ctl, &dev) >= 0 && dev >= 0) {
            snd_pcm_info_t *pcmInfo;
            snd_pcm_info_alloca(&pcmInfo);
            snd_pcm_info_set_device(pcmInfo, dev);
            snd_pcm_info_set_subdevice(pcmInfo, 0);
            snd_pcm_info_set_stream(pcmInfo, stream);
            if (snd_ctl_pcm_info(ctl, pcmInfo) < 0) continue; // this device doesn't support this direction

            QString name = QString("plughw:%1,%2").arg(card).arg(dev);
            QString desc = QString("card %1: %2 [%3], device %4: %5")
                                .arg(card)
                                .arg(cardId)
                                .arg(cardName)
                                .arg(dev)
                                .arg(QString::fromUtf8(snd_pcm_info_get_name(pcmInfo)));
            out << AlsaDeviceInfo{name, desc};
        }
        snd_ctl_close(ctl);
    }
    return out;
}

// /dev/serial/by-id gives stable names for USB-serial adapters (survives
// port renumbering across reboots/replugs) -- exactly what HowToStart.md
// tells users to look under for the ThumbDV.
QStringList listSerialByIdDevices() {
    QStringList out;
    QDir dir("/dev/serial/by-id");
    for (const QString &name : dir.entryList(QDir::NoDotAndDotDot | QDir::System | QDir::Files))
        out << dir.filePath(name);
    return out;
}

// Editable combo whose dropdown shows "name -- description" for
// discoverability (aplay -L/arecord -L style) but whose committed text
// (what the rest of the app reads back via currentText()) is always the
// plain device name -- the description is only ever navigation aid, never
// part of the value.
QComboBox *makeAlsaDeviceCombo(const QList<AlsaDeviceInfo> &devices, const QString &current) {
    auto *combo = new QComboBox;
    combo->setEditable(true);

    bool haveCurrent = false;
    for (const AlsaDeviceInfo &d : devices) {
        QString label = d.name;
        if (!d.desc.isEmpty()) label += "  --  " + d.desc;
        combo->addItem(label, d.name);
        if (d.name == current) haveCurrent = true;
    }
    if (!haveCurrent) combo->addItem(current, current);

    QObject::connect(combo, &QComboBox::activated, combo, [combo](int index) {
        combo->setEditText(combo->itemData(index).toString());
    });

    combo->setCurrentText(current);
    return combo;
}

QComboBox *makeEditableCombo(const QStringList &items, const QString &current) {
    auto *combo = new QComboBox;
    combo->setEditable(true);
    combo->addItems(items);
    if (!items.contains(current)) combo->addItem(current);
    combo->setCurrentText(current);
    return combo;
}

// Opens `device` and plays one second of a 440Hz (concert A) sine wave --
// just enough to let the user confirm they picked the right output
// device, same 8kHz/16-bit-mono format as everything else this app plays.
// Blocks for the duration (AlsaPcm::write() paces itself to real time),
// so always call this from a background thread, never the GUI thread.
void playTestTone(const std::string &device) {
    dextra::AlsaPcm pcm;
    if (!pcm.open(device, SND_PCM_STREAM_PLAYBACK)) return;

    constexpr double kFrequencyHz = 440.0;
    constexpr double kSampleRateHz = 8000.0;
    constexpr double kDurationSec = 1.0;
    constexpr double kTwoPi = 6.283185307179586;
    constexpr short kAmplitude = 8000; // well under full-scale (32767) -- a loud tone helps nobody

    double phase = 0.0;
    const double phaseStep = kTwoPi * kFrequencyHz / kSampleRateHz;
    size_t samplesWritten = 0;
    const size_t totalSamples = static_cast<size_t>(kSampleRateHz * kDurationSec);
    while (samplesWritten < totalSamples) {
        short chunk[SerialDV::MBE_AUDIO_BLOCK_SIZE];
        for (short &sample : chunk) {
            sample = static_cast<short>(kAmplitude * std::sin(phase));
            phase += phaseStep;
            if (phase >= kTwoPi) phase -= kTwoPi;
        }
        if (!pcm.write(chunk)) break;
        samplesWritten += SerialDV::MBE_AUDIO_BLOCK_SIZE;
    }
    pcm.close();
}

// Peak sample magnitude in `chunk`, as a percentage of full scale (0-100)
// -- simplest useful level measure for a "am I picking up sound" meter;
// RMS would read steadier but peak is plenty for this.
int peakLevelPercent(const short pcm[SerialDV::MBE_AUDIO_BLOCK_SIZE]) {
    int peak = 0;
    for (unsigned i = 0; i < SerialDV::MBE_AUDIO_BLOCK_SIZE; i++) peak = std::max(peak, std::abs(static_cast<int>(pcm[i])));
    return static_cast<int>(peak * 100 / 32767);
}

} // namespace

SettingsDialog::SettingsDialog(const GuiSettings &current, QWidget *parent) : QDialog(parent), m_initial(current) {
    setWindowTitle("Settings");

    m_callsign = new QLineEdit(current.callsign);
    m_callsign->setMaxLength(7); // 8-byte protocol field minus the trailing module byte

    m_suffix = new QLineEdit(current.suffix);
    m_suffix->setMaxLength(4); // MYCALL2: a separate 4-byte header field, distinct from the module byte below

    m_moduleSuffix = new QComboBox;
    for (char c = 'A'; c <= 'Z'; c++) m_moduleSuffix->addItem(QString(QChar(c)));
    m_moduleSuffix->setCurrentText(current.moduleSuffix);

    m_audioInput = makeAlsaDeviceCombo(listAlsaHardwareDevices(SND_PCM_STREAM_CAPTURE), current.audioInputDevice);
    m_audioOutput = makeAlsaDeviceCombo(listAlsaHardwareDevices(SND_PCM_STREAM_PLAYBACK), current.audioOutputDevice);

    // Toggle, not one-shot -- unlike the output tone (a fixed clip), input
    // testing is about watching live levels while the user makes noise, so
    // it runs until explicitly stopped (or the dialog closes -- see
    // stopAudioInputTest()). Styled as a rough VU meter: instant rise, a
    // slow step-decay per update (real ALSA reads pace this to ~20ms, so
    // decaying 5/update reads as a fairly natural falloff) rather than
    // jumping straight to each new peak, and a green/amber/red gradient
    // chunk so the meter reads at a glance without needing numbers.
    m_audioInputLevel = new QProgressBar;
    m_audioInputLevel->setRange(0, 100);
    m_audioInputLevel->setTextVisible(false);
    m_audioInputLevel->setStyleSheet(
        "QProgressBar { border: 1px solid gray; background: qlineargradient(x1:0, y1:0, x2:1, y2:0,"
        "  stop:0 #2e6b32, stop:0.7 #2e6b32, stop:0.85 #8a5a00, stop:1 #8a2424); }"
        "QProgressBar::chunk { background: qlineargradient(x1:0, y1:0, x2:1, y2:0,"
        "  stop:0 #4CAF50, stop:0.7 #4CAF50, stop:0.85 #ff9800, stop:1 #f44336); }");

    m_audioInputTest = new QPushButton("Test");
    m_audioInputTest->setCheckable(true);
    connect(m_audioInputTest, &QPushButton::toggled, this, [this](bool testing) {
        if (!testing) {
            stopAudioInputTest();
            return;
        }
        m_audioInputTest->setText("Stop");
        m_audioInput->setEnabled(false);
        m_audioInputTesting.store(true);
        std::string device = m_audioInput->currentText().trimmed().toStdString();
        m_audioInputTestThread = std::thread([this, device] {
            dextra::AlsaPcm pcm;
            if (!pcm.open(device, SND_PCM_STREAM_CAPTURE)) {
                m_audioInputTesting.store(false);
                return;
            }
            // Same fast-read throttle as dextra::captureThread (see its
            // comments for the full reasoning) -- the "null" device this
            // combo box explicitly offers never blocks, so without this an
            // active test against it would spin a CPU core at 100%.
            constexpr auto period = std::chrono::milliseconds(20);
            constexpr auto fastThreshold = std::chrono::milliseconds(2);
            constexpr int fastStreakLimit = 5;
            int fastReadStreak = 0;
            short chunk[SerialDV::MBE_AUDIO_BLOCK_SIZE];
            while (m_audioInputTesting.load()) {
                auto readStart = std::chrono::steady_clock::now();
                bool gotAudio = pcm.read(chunk);
                auto readElapsed = std::chrono::steady_clock::now() - readStart;

                if (readElapsed < fastThreshold) {
                    if (fastReadStreak < fastStreakLimit) fastReadStreak++;
                } else {
                    fastReadStreak = 0;
                }
                if (fastReadStreak >= fastStreakLimit) std::this_thread::sleep_for(period);

                if (!gotAudio) continue;
                int level = peakLevelPercent(chunk);
                QMetaObject::invokeMethod(
                    this,
                    [this, level] {
                        // Guards against a real race: this update was
                        // already queued (Qt::QueuedConnection doesn't run
                        // it immediately) when the user clicked Stop.
                        // stopAudioInputTest() resets the meter to 0
                        // synchronously, but that reset happens before
                        // control returns to the event loop -- without this
                        // check, a stale update sitting in the queue would
                        // then run right after and undo it, leaving the
                        // meter stuck showing the last level captured.
                        if (!m_audioInputTesting.load()) return;
                        int decayed = std::max(0, m_audioInputLevel->value() - 5);
                        m_audioInputLevel->setValue(std::max(level, decayed));
                    },
                    Qt::QueuedConnection);
            }
            pcm.close();
        });
    });

    // Closing the dialog (OK, Cancel, or the window's own close button --
    // QDialog::finished() covers all three) while a test is still running
    // must not leave the capture thread pointed at a widget that's about
    // to be destroyed.
    connect(this, &QDialog::finished, this, [this](int) { stopAudioInputTest(); });

    // Plays a one-second 440Hz test tone through whatever's currently
    // selected above (even if not yet saved) so the user can confirm
    // they picked the right output before hitting OK. Disabled for the
    // tone's duration to avoid two overlapping playTestTone() calls
    // fighting over the same device.
    m_audioOutputTest = new QPushButton("Test");
    connect(m_audioOutputTest, &QPushButton::clicked, this, [this] {
        m_audioOutputTest->setEnabled(false);
        std::string device = m_audioOutput->currentText().trimmed().toStdString();
        std::thread([this, device] {
            playTestTone(device);
            QMetaObject::invokeMethod(
                this,
                [this] {
                    // Disabling a focused widget hands focus to the next one
                    // in tab order (here, the ThumbDV combo) and re-enabling
                    // it doesn't hand focus back on its own -- restore it
                    // explicitly so Test doesn't feel like it silently
                    // shoves focus elsewhere.
                    m_audioOutputTest->setEnabled(true);
                    m_audioOutputTest->setFocus();
                },
                Qt::QueuedConnection);
        }).detach();
    });

    m_thumbdv = makeEditableCombo(listSerialByIdDevices(), current.thumbdvDevice);

    m_thumbdvMode = new QComboBox;
    m_thumbdvMode->addItem("Local serial device", "serial");
    m_thumbdvMode->addItem("Network (AMBEServer 3000)", "network");
    m_thumbdvMode->setCurrentIndex(current.thumbdvIsNetwork() ? 1 : 0);

    m_thumbdvHost = new QLineEdit(current.thumbdvHost);
    m_thumbdvHost->setPlaceholderText("hostname or IP of the machine running AMBEServer");
    m_thumbdvPort = new QLineEdit(QString::number(current.thumbdvPort));
    m_thumbdvPort->setValidator(new QIntValidator(1, 65535, m_thumbdvPort));
    m_thumbdvPort->setMaximumWidth(80);

    // Opens a real DVController against the typed host:port (even if not yet
    // saved) so a wrong address, a firewall, or a stopped server shows up here
    // instead of as a failed Connect. The controller is created on the worker
    // thread and the result posted back through a QPointer, the same shape as
    // the master-list fetch: the dialog may be closed while it waits.
    m_thumbdvTest = new QPushButton("Test");
    m_thumbdvTestResult = new QLabel;
    connect(m_thumbdvTest, &QPushButton::clicked, this, [this] {
        GuiSettings probe;
        probe.thumbdvMode = "network";
        probe.thumbdvHost = m_thumbdvHost->text().trimmed();
        probe.thumbdvPort = m_thumbdvPort->text().toInt();
        const QString target = probe.thumbdvTarget();
        if (target.isEmpty() || probe.thumbdvPort < 1) {
            m_thumbdvTestResult->setText("Enter a host and port first.");
            return;
        }
        m_thumbdvTest->setEnabled(false);
        m_thumbdvTestResult->setText("Testing " + target + "...");
        QPointer<SettingsDialog> self(this);
        std::thread([self, target] {
            SerialDV::DVController dv;
            const bool ok = dv.open(target.toStdString());
            if (ok) dv.close();
            QMetaObject::invokeMethod(
                qApp,
                [self, target, ok] {
                    if (!self) return;
                    self->m_thumbdvTest->setEnabled(true);
                    self->m_thumbdvTestResult->setText(
                        ok ? "OK: the ThumbDV at " + target + " responded."
                           : "No response from " + target + " (server stopped, wrong address, or firewall?).");
                },
                Qt::QueuedConnection);
        }).detach();
    });

    m_dmrId = new QLineEdit(current.dmrId ? QString::number(current.dmrId) : QString());
    m_dmrId->setValidator(new QIntValidator(0, 99999999, m_dmrId)); // DMR IDs are up to 8 digits
    m_dmrId->setPlaceholderText("e.g. your radioid.net-registered ID");

    m_dmrIdSuffix = new QLineEdit(current.dmrIdSuffix);
    m_dmrIdSuffix->setMaxLength(2);
    m_dmrIdSuffix->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-9]{0,2}"), m_dmrIdSuffix));
    m_dmrIdSuffix->setPlaceholderText("optional, 2 digits");
    m_dmrIdSuffix->setToolTip("Appended to the DMR ID above to form a 9-digit repeater ID. Needed when another hotspot or client "
                              "(e.g. BlueDV) is connected under the same DMR ID at the same time -- each simultaneous "
                              "connection must have a unique ID. Leave blank to use the plain DMR ID.");

    // Each DMR network has its own server and its own password, and each
    // is reached over its own protocol (BrandMeister: Open DMR Terminal,
    // TGIF: Homebrew) -- so choosing the network is all that's needed, and
    // only that network's fields are shown (both sets are kept and saved,
    // so you can configure both and pick one on the DMR tab).
    m_dmrNetwork = new QComboBox;
    m_dmrNetwork->addItem("BrandMeister", "brandmeister");
    m_dmrNetwork->addItem("TGIF", "tgif");
    m_dmrNetwork->setCurrentIndex(current.dmrNetwork == "tgif" ? 1 : 0);

    // BrandMeister's masters as a dropdown ("AU 5051"), like BlueDV. Editable
    // so a hostname can still be typed -- an older saved value that isn't in
    // the list, or when the list can't be fetched (offline, first run).
    m_bmServer = new QComboBox;
    m_bmServer->setEditable(true);
    m_bmServer->setInsertPolicy(QComboBox::NoInsert);
    m_bmServer->lineEdit()->setPlaceholderText("pick a master, or type a hostname -- the port is fixed");
    if (!current.bmServer.isEmpty()) m_bmServer->setEditText(current.bmServer);

    // The last cached list shows straight away; if it's stale (or there is
    // none yet), refresh it in the background. The dialog may be closed
    // before that returns, so the result goes through a guarded pointer.
    {
        std::vector<bmmaster::MasterInfo> cached;
        if (bmmaster::loadCachedMasterList(cached)) setBmMasters(cached);
        if (!bmmaster::isMasterCacheFresh(cache::ONE_DAY_SECONDS)) {
            QPointer<SettingsDialog> guard(this);
            std::thread([guard] {
                std::vector<bmmaster::MasterInfo> live;
                QString error;
                if (!bmmaster::fetchMasterList(live, error)) return; // keep whatever's showing
                QMetaObject::invokeMethod(
                    qApp, [guard, live = std::move(live)] {
                        if (guard) guard->setBmMasters(live);
                    },
                    Qt::QueuedConnection);
            }).detach();
        }
    }
    m_bmPassword = new QLineEdit(current.bmPassword);
    m_bmPassword->setEchoMode(QLineEdit::Password);
    m_bmPassword->setToolTip("The \"Hotspot Security\" password you set in BrandMeister SelfCare -- not your account password.");

    m_tgifServer = new QLineEdit(current.tgifServer);
    m_tgifServer->setPlaceholderText("tgif.network -- the port is fixed, don't add one");
    m_tgifPassword = new QLineEdit(current.tgifPassword);
    m_tgifPassword->setEchoMode(QLineEdit::Password);
    m_tgifPassword->setToolTip("The Hotspot Security Key (16 digits) generated on your TGIF account's security page.");

    // RPTC config fields -- see settings.h. (Frequency, color code and time
    // slot are RF-only and hardcoded in RepeaterConfig, so not here.)
    m_dmrLatitude = new QLineEdit(QString::number(current.dmrLatitude, 'f', 4));
    m_dmrLatitude->setValidator(new QDoubleValidator(-90.0, 90.0, 4, m_dmrLatitude));

    m_dmrLongitude = new QLineEdit(QString::number(current.dmrLongitude, 'f', 4));
    m_dmrLongitude->setValidator(new QDoubleValidator(-180.0, 180.0, 4, m_dmrLongitude));

    m_dmrLocation = new QLineEdit(current.dmrLocation);
    m_dmrLocation->setMaxLength(20); // RPTC's Location field is a fixed 20 bytes

    m_dmrDescription = new QLineEdit(current.dmrDescription);
    m_dmrDescription->setMaxLength(19); // RPTC's Description field is a fixed 19 bytes

    m_dmrUrl = new QLineEdit(current.dmrUrl);
    m_dmrUrl->setPlaceholderText("optional, e.g. a page about your station");

    // Callsign is shared (D-Star identity, and what the DMR RPTC declares),
    // so it sits above the two protocol groups.
    auto *callsignForm = new QFormLayout;
    callsignForm->addRow("Callsign:", m_callsign);

    auto *dstarBox = new QGroupBox("D-Star");
    auto *dstarForm = new QFormLayout(dstarBox);
    dstarForm->addRow("Suffix:", m_suffix);
    dstarForm->addRow("Module suffix:", m_moduleSuffix);

    auto *dmrBox = new QGroupBox("DMR");
    auto *dmrForm = new QFormLayout(dmrBox);
    dmrForm->addRow("DMR ID:", m_dmrId);
    dmrForm->addRow("DMR network:", m_dmrNetwork);

    // One page of fields per network, only the chosen one shown.
    auto *bmForm = new QFormLayout;
    bmForm->addRow("BrandMeister server:", m_bmServer);
    bmForm->addRow("Hotspot Security password:", m_bmPassword);
    auto *bmPage = new QWidget;
    bmPage->setLayout(bmForm);
    bmForm->setContentsMargins(0, 0, 0, 0); // the DMR box already provides the margins

    auto *tgifForm = new QFormLayout;
    tgifForm->addRow("TGIF server:", m_tgifServer);
    tgifForm->addRow("Hotspot Security Key:", m_tgifPassword);
    tgifForm->addRow("DMR ID suffix:", m_dmrIdSuffix);
    tgifForm->addRow("Latitude:", m_dmrLatitude);
    tgifForm->addRow("Longitude:", m_dmrLongitude);
    tgifForm->addRow("Location:", m_dmrLocation);
    tgifForm->addRow("Description:", m_dmrDescription);
    tgifForm->addRow("URL:", m_dmrUrl);
    auto *tgifPage = new QWidget;
    tgifPage->setLayout(tgifForm);
    tgifForm->setContentsMargins(0, 0, 0, 0);

    m_dmrNetworkPages = new QStackedWidget;
    m_dmrNetworkPages->addWidget(bmPage);
    m_dmrNetworkPages->addWidget(tgifPage);
    dmrForm->addRow(m_dmrNetworkPages);
    connect(m_dmrNetwork, &QComboBox::currentIndexChanged, this, &SettingsDialog::updateDmrNetworkPage);

    auto *generalLayout = new QVBoxLayout;
    generalLayout->addLayout(callsignForm);
    generalLayout->addWidget(dstarBox);
    generalLayout->addWidget(dmrBox);
    generalLayout->addStretch();
    auto *generalPage = new QWidget;
    generalPage->setLayout(generalLayout);

    updateDmrNetworkPage();

    // Fixed, matched width for both Test buttons so the two device rows
    // -- and their dropdowns -- line up. Measured against "Stop" (not
    // just the initial "Test" label) since m_audioInputTest toggles to
    // that and must not change width when it does.
    m_audioInputTest->setText("Stop");
    int testButtonWidth = std::max(m_audioInputTest->sizeHint().width(),
                                    m_audioOutputTest->sizeHint().width());
    m_audioInputTest->setText("Test");
    m_audioInputTest->setFixedWidth(testButtonWidth);
    m_audioOutputTest->setFixedWidth(testButtonWidth);

    auto *audioInputRow = new QHBoxLayout;
    audioInputRow->addWidget(m_audioInput, 1);
    audioInputRow->addWidget(m_audioInputTest);
    auto *audioInputColumn = new QVBoxLayout;
    audioInputColumn->addLayout(audioInputRow);
    audioInputColumn->addWidget(m_audioInputLevel);

    auto *audioOutputRow = new QHBoxLayout;
    audioOutputRow->addWidget(m_audioOutput, 1);
    audioOutputRow->addWidget(m_audioOutputTest);

    auto *serialForm = new QFormLayout;
    serialForm->setContentsMargins(0, 0, 0, 0);
    serialForm->addRow("Serial device:", m_thumbdv);
    auto *serialPage = new QWidget;
    serialPage->setLayout(serialForm);

    auto *thumbdvTestRow = new QHBoxLayout;
    thumbdvTestRow->addWidget(m_thumbdvTest);
    thumbdvTestRow->addWidget(m_thumbdvTestResult, 1);
    auto *networkForm = new QFormLayout;
    networkForm->setContentsMargins(0, 0, 0, 0);
    networkForm->addRow("Host:", m_thumbdvHost);
    networkForm->addRow("Port:", m_thumbdvPort);
    networkForm->addRow(thumbdvTestRow);
    auto *networkPage = new QWidget;
    networkPage->setLayout(networkForm);

    // Same QStackedWidget shape as m_dmrNetworkPages below, rather than one
    // shared form with rows shown/hidden per mode (QFormLayout::
    // setRowVisible(), used here previously) -- that alternative is what a
    // git-bisect traced a real GNOME/Wayland bug to: Qt's client-side
    // Adwaita decoration spuriously trying (and Mutter rightly refusing) to
    // minimize this dialog the moment it opens, which manifests as a
    // system-wide stuck busy cursor for a while. The stacked-page shape is
    // what the DMR network fields already use safely below, so this trades
    // the Host:/Port: labels no longer lining up with the audio labels
    // above for staying on the known-good pattern.
    m_thumbdvPages = new QStackedWidget;
    m_thumbdvPages->addWidget(serialPage);
    m_thumbdvPages->addWidget(networkPage);
    connect(m_thumbdvMode, &QComboBox::currentIndexChanged, this, &SettingsDialog::updateThumbdvPage);
    updateThumbdvPage();

    auto *devicesForm = new QFormLayout;
    devicesForm->addRow("Audio input device:", audioInputColumn);
    devicesForm->addRow("Audio output device:", audioOutputRow);
    devicesForm->addRow("ThumbDV:", m_thumbdvMode);
    devicesForm->addRow(m_thumbdvPages);

    // The label columns of devicesForm, serialForm and networkForm are
    // three independent QFormLayouts (serialForm/networkForm are nested a
    // level down, inside m_thumbdvPages' pages), so each would otherwise
    // size its own label column to just its own labels -- leaving Serial
    // device:/Host:/Port: sitting well left of Audio input device: etc.
    // Pin every label in all three to one shared width instead, wide
    // enough for the longest ("Audio output device:"), so they read as one
    // column without needing them to actually share a layout (see the
    // QStackedWidget comment above for why they don't).
    int labelWidth = 0;
    QList<QFormLayout *> allForms = {devicesForm, serialForm, networkForm};
    QList<QLabel *> allLabels;
    for (QFormLayout *form : allForms) {
        for (int row = 0; row < form->rowCount(); row++) {
            if (auto *item = form->itemAt(row, QFormLayout::LabelRole)) {
                if (auto *label = qobject_cast<QLabel *>(item->widget())) {
                    allLabels << label;
                    labelWidth = std::max(labelWidth, label->sizeHint().width());
                }
            }
        }
    }
    for (QLabel *label : allLabels) label->setFixedWidth(labelWidth);

    auto *devicesPage = new QWidget;
    devicesPage->setLayout(devicesForm);

    auto *tabs = new QTabWidget;
    tabs->addTab(generalPage, "General");
    tabs->addTab(devicesPage, "Devices");

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(tabs);
    layout->addWidget(buttons);
}

void SettingsDialog::updateThumbdvPage() {
    int index = m_thumbdvMode->currentIndex();
    m_thumbdvPages->setCurrentIndex(index);
    // Same reasoning as updateDmrNetworkPage(): only the shown page should
    // count toward the stack's size, so the dialog can shrink back for the
    // shorter serial page after showing the taller network one.
    for (int i = 0; i < m_thumbdvPages->count(); i++) {
        m_thumbdvPages->widget(i)->setSizePolicy(i == index ? QSizePolicy::Preferred : QSizePolicy::Ignored,
                                                  i == index ? QSizePolicy::Preferred : QSizePolicy::Ignored);
    }
    m_thumbdvPages->updateGeometry();
    QTimer::singleShot(0, this, &QWidget::adjustSize);
}

void SettingsDialog::stopAudioInputTest() {
    m_audioInputTesting.store(false);
    if (m_audioInputTestThread.joinable()) m_audioInputTestThread.join();
    m_audioInputTest->setChecked(false); // a no-op (no re-entrant toggled()) if already unchecked
    m_audioInputTest->setText("Test");
    m_audioInput->setEnabled(true);
    m_audioInputLevel->setValue(0);
}

QString SettingsDialog::bmServerHost() const {
    QString text = m_bmServer->currentText().trimmed();
    int index = m_bmServer->findText(text);
    return index >= 0 ? m_bmServer->itemData(index).toString() : text;
}

void SettingsDialog::setBmMasters(const std::vector<bmmaster::MasterInfo> &masters) {
    QString host = bmServerHost(); // what's chosen now, to put back afterwards
    m_bmServer->clear();
    for (const bmmaster::MasterInfo &m : masters) m_bmServer->addItem(m.label(), m.host());
    int index = m_bmServer->findData(host);
    if (index >= 0) m_bmServer->setCurrentIndex(index);
    else m_bmServer->setEditText(host); // not in the list: keep it as typed
}

void SettingsDialog::updateDmrNetworkPage() {
    int index = m_dmrNetwork->currentIndex();
    m_dmrNetworkPages->setCurrentIndex(index);
    // A QStackedWidget is as big as its biggest page, which would leave the
    // short BrandMeister page floating in the TGIF page's height -- let the
    // hidden page stop counting toward the size.
    for (int i = 0; i < m_dmrNetworkPages->count(); i++) {
        m_dmrNetworkPages->widget(i)->setSizePolicy(i == index ? QSizePolicy::Preferred : QSizePolicy::Ignored,
                                                     i == index ? QSizePolicy::Preferred : QSizePolicy::Ignored);
    }
    m_dmrNetworkPages->updateGeometry();
    // Resize once the layouts have caught up -- the forms inside the pages
    // only recompute their size hints on the next event-loop turn, so an
    // immediate adjustSize() runs against stale (still-large) minimums and
    // the dialog grows for the long TGIF page but never shrinks back for
    // BrandMeister's.
    QTimer::singleShot(0, this, &QWidget::adjustSize);
}

GuiSettings SettingsDialog::settings() const {
    GuiSettings s = m_initial;
    s.callsign = m_callsign->text().trimmed().toUpper();
    s.suffix = m_suffix->text().trimmed().toUpper();
    s.moduleSuffix = m_moduleSuffix->currentText();
    s.audioInputDevice = m_audioInput->currentText().trimmed();
    s.audioOutputDevice = m_audioOutput->currentText().trimmed();
    s.thumbdvMode = m_thumbdvMode->currentData().toString();
    s.thumbdvDevice = m_thumbdv->currentText().trimmed();
    s.thumbdvHost = m_thumbdvHost->text().trimmed();
    s.thumbdvPort = qBound(1, m_thumbdvPort->text().toInt(), 65535);
    s.dmrId = static_cast<uint32_t>(m_dmrId->text().trimmed().toULong());
    s.dmrIdSuffix = m_dmrIdSuffix->text().trimmed();
    // dmrNetwork is deliberately left as opened: the combo here only picks
    // which network's fields to edit; the DMR tab's own network combo decides
    // which one is used.
    s.bmServer = bmServerHost();
    s.bmPassword = m_bmPassword->text();
    s.tgifServer = m_tgifServer->text().trimmed();
    s.tgifPassword = m_tgifPassword->text();
    s.dmrLatitude = m_dmrLatitude->text().toDouble();
    s.dmrLongitude = m_dmrLongitude->text().toDouble();
    s.dmrLocation = m_dmrLocation->text().trimmed();
    s.dmrDescription = m_dmrDescription->text().trimmed();
    s.dmrUrl = m_dmrUrl->text().trimmed();
    return s;
}
