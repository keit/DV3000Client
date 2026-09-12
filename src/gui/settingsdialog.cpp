#include "settingsdialog.h"

#include <alsa/asoundlib.h>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QLineEdit>
#include <QVBoxLayout>

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

    m_thumbdv = makeEditableCombo(listSerialByIdDevices(), current.thumbdvDevice);

    auto *form = new QFormLayout;
    form->addRow("Callsign:", m_callsign);
    form->addRow("Suffix:", m_suffix);
    form->addRow("Module suffix:", m_moduleSuffix);
    form->addRow("Audio input device:", m_audioInput);
    form->addRow("Audio output device:", m_audioOutput);
    form->addRow("ThumbDV device:", m_thumbdv);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);
}

GuiSettings SettingsDialog::settings() const {
    GuiSettings s;
    s.callsign = m_callsign->text().trimmed().toUpper();
    s.suffix = m_suffix->text().trimmed().toUpper();
    s.moduleSuffix = m_moduleSuffix->currentText();
    s.audioInputDevice = m_audioInput->currentText().trimmed();
    s.audioOutputDevice = m_audioOutput->currentText().trimmed();
    s.thumbdvDevice = m_thumbdv->currentText().trimmed();
    return s;
}
