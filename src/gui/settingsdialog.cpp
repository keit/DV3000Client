#include "settingsdialog.h"

#include <alsa/asoundlib.h>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleValidator>
#include <QFormLayout>
#include <QIntValidator>
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

    m_dmrId = new QLineEdit(current.dmrId ? QString::number(current.dmrId) : QString());
    m_dmrId->setValidator(new QIntValidator(0, 99999999, m_dmrId)); // DMR IDs are up to 8 digits
    m_dmrId->setPlaceholderText("e.g. your radioid.net-registered ID");

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

    auto *form = new QFormLayout;
    form->addRow("Callsign:", m_callsign);
    form->addRow("Suffix:", m_suffix);
    form->addRow("Module suffix:", m_moduleSuffix);
    form->addRow("Audio input device:", m_audioInput);
    form->addRow("Audio output device:", m_audioOutput);
    form->addRow("ThumbDV device:", m_thumbdv);
    form->addRow("DMR ID:", m_dmrId);
    form->addRow("DMR password:", m_dmrPassword);
    form->addRow("DMR server:", m_dmrServer);
    form->addRow("DMR color code:", m_dmrColorCode);
    form->addRow("DMR time slot:", m_dmrTimeSlot);
    form->addRow("DMR frequency (MHz):", m_dmrFrequencyMhz);
    form->addRow("DMR latitude:", m_dmrLatitude);
    form->addRow("DMR longitude:", m_dmrLongitude);
    form->addRow("DMR location:", m_dmrLocation);
    form->addRow("DMR description:", m_dmrDescription);
    form->addRow("DMR URL:", m_dmrUrl);

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
    s.dmrId = static_cast<uint32_t>(m_dmrId->text().trimmed().toULong());
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
