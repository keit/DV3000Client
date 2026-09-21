#include "settingsdialog.h"

#include <alsa/asoundlib.h>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleValidator>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLineEdit>
#include <QMetaObject>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>

#include "dextra_audio.h"

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

SettingsDialog::SettingsDialog(const GuiSettings &current, QWidget *parent) : QDialog(parent) {
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

    m_dmrPassword = new QLineEdit(current.dmrPassword);
    m_dmrPassword->setEchoMode(QLineEdit::Password);

    m_dmrServer = new QLineEdit(current.dmrServer);
    m_dmrServer->setPlaceholderText("host:port, e.g. 3101.brandmeister.network:62031");

    m_dmrColorCode = new QComboBox;
    for (int cc = 0; cc <= 15; cc++) m_dmrColorCode->addItem(QString::number(cc), cc);
    m_dmrColorCode->setCurrentIndex(static_cast<int>(current.dmrColorCode));

    // Slot 2 is the confirmed convention for hotspot-style BrandMeister
    // connections -- see settings.h's dmrTimeSlot comment.
    m_dmrTimeSlot = new QComboBox;
    m_dmrTimeSlot->addItem("1", 1);
    m_dmrTimeSlot->addItem("2", 2);
    m_dmrTimeSlot->setCurrentIndex(current.dmrTimeSlot == 1 ? 0 : 1);

    // RPTC config fields -- see settings.h's comment on why these matter
    // for real masters (BrandMeister) even though xlxd barely checks them.
    // Defaults are generic placeholders; fill in your actual repeater/
    // hotspot details here (the same numbers Pi-Star or BlueDV already
    // has, if you run those) if a real master rejects the configuration
    // step with generic values.
    m_dmrFrequencyMhz = new QLineEdit(QString::number(current.dmrFrequencyMhz, 'f', 6));
    m_dmrFrequencyMhz->setValidator(new QDoubleValidator(0.0, 9999.0, 6, m_dmrFrequencyMhz));
    m_dmrFrequencyMhz->setPlaceholderText("e.g. 438.325000 -- used as both RX and TX (simplex)");

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

    auto *generalForm = new QFormLayout;
    generalForm->addRow("Callsign:", m_callsign);
    generalForm->addRow("Suffix:", m_suffix);
    generalForm->addRow("Module suffix:", m_moduleSuffix);
    generalForm->addRow("DMR ID:", m_dmrId);
    generalForm->addRow("DMR ID suffix:", m_dmrIdSuffix);
    generalForm->addRow("DMR password:", m_dmrPassword);
    generalForm->addRow("DMR server:", m_dmrServer);
    generalForm->addRow("DMR color code:", m_dmrColorCode);
    generalForm->addRow("DMR time slot:", m_dmrTimeSlot);
    generalForm->addRow("DMR frequency (MHz):", m_dmrFrequencyMhz);
    generalForm->addRow("DMR latitude:", m_dmrLatitude);
    generalForm->addRow("DMR longitude:", m_dmrLongitude);
    generalForm->addRow("DMR location:", m_dmrLocation);
    generalForm->addRow("DMR description:", m_dmrDescription);
    generalForm->addRow("DMR URL:", m_dmrUrl);
    auto *generalPage = new QWidget;
    generalPage->setLayout(generalForm);

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

    auto *devicesForm = new QFormLayout;
    devicesForm->addRow("Audio input device:", audioInputColumn);
    devicesForm->addRow("Audio output device:", audioOutputRow);
    devicesForm->addRow("ThumbDV device:", m_thumbdv);
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

void SettingsDialog::stopAudioInputTest() {
    m_audioInputTesting.store(false);
    if (m_audioInputTestThread.joinable()) m_audioInputTestThread.join();
    m_audioInputTest->setChecked(false); // a no-op (no re-entrant toggled()) if already unchecked
    m_audioInputTest->setText("Test");
    m_audioInput->setEnabled(true);
    m_audioInputLevel->setValue(0);
}

GuiSettings SettingsDialog::settings() const {
    GuiSettings s;
    s.callsign = m_callsign->text().trimmed().toUpper();
    s.suffix = m_suffix->text().trimmed().toUpper();
    s.moduleSuffix = m_moduleSuffix->currentText();
    s.audioInputDevice = m_audioInput->currentText().trimmed();
    s.audioOutputDevice = m_audioOutput->currentText().trimmed();
    s.thumbdvDevice = m_thumbdv->currentText().trimmed();
    s.dmrId = static_cast<uint32_t>(m_dmrId->text().trimmed().toULong());
    s.dmrIdSuffix = m_dmrIdSuffix->text().trimmed();
    s.dmrPassword = m_dmrPassword->text();
    s.dmrServer = m_dmrServer->text().trimmed();
    s.dmrColorCode = static_cast<unsigned>(m_dmrColorCode->currentIndex());
    s.dmrTimeSlot = static_cast<unsigned>(m_dmrTimeSlot->currentData().toInt());
    s.dmrFrequencyMhz = m_dmrFrequencyMhz->text().toDouble();
    s.dmrLatitude = m_dmrLatitude->text().toDouble();
    s.dmrLongitude = m_dmrLongitude->text().toDouble();
    s.dmrLocation = m_dmrLocation->text().trimmed();
    s.dmrDescription = m_dmrDescription->text().trimmed();
    s.dmrUrl = m_dmrUrl->text().trimmed();
    return s;
}
