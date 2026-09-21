#pragma once

// Modal settings dialog, split across two tabs: "General" (callsign,
// MYCALL2 suffix, our own module letter, and the DMR/BrandMeister
// identity -- DMR ID, hotspot password, server, color code -- used by the
// DMR tab) and "Devices" (audio in/out device, ThumbDV serial device).
// The device dropdowns are pre-populated by probing ALSA and
// /dev/serial/by-id (see .cpp) but stay editable, since not every valid
// device name shows up in either probe.

#include <QDialog>

#include "settings.h"

class QComboBox;
class QLineEdit;
class QPushButton;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(const GuiSettings &current, QWidget *parent = nullptr);

    GuiSettings settings() const;

private:
    QLineEdit *m_callsign;
    QLineEdit *m_suffix;
    QComboBox *m_moduleSuffix;
    QComboBox *m_audioInput;
    QComboBox *m_audioOutput;
    QPushButton *m_audioOutputTest;
    QComboBox *m_thumbdv;
    QLineEdit *m_dmrId;
    QLineEdit *m_dmrIdSuffix;
    QLineEdit *m_dmrPassword;
    QLineEdit *m_dmrServer;
    QComboBox *m_dmrColorCode;
    QComboBox *m_dmrTimeSlot;
    QLineEdit *m_dmrFrequencyMhz;
    QLineEdit *m_dmrLatitude;
    QLineEdit *m_dmrLongitude;
    QLineEdit *m_dmrLocation;
    QLineEdit *m_dmrDescription;
    QLineEdit *m_dmrUrl;
};
