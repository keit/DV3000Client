#pragma once

// Modal settings dialog, split across two tabs: "General" (callsign,
// MYCALL2 suffix, our own module letter, and the DMR/BrandMeister
// identity -- DMR ID, hotspot password, server, color code -- used by the
// DMR tab) and "Devices" (audio in/out device, ThumbDV serial device).
// The device dropdowns are pre-populated by probing ALSA and
// /dev/serial/by-id (see .cpp) but stay editable, since not every valid
// device name shows up in either probe.

#include <QDialog>

#include <atomic>
#include <thread>

#include "settings.h"

class QComboBox;
class QLineEdit;
class QProgressBar;
class QPushButton;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(const GuiSettings &current, QWidget *parent = nullptr);

    GuiSettings settings() const;

private:
    // Stops the input-level capture thread if running (sets the flag and
    // joins -- near-instant, since each ALSA read the thread does only
    // blocks for one ~20ms period) and resets the Test button/meter.
    // Called both when the user clicks Stop and, via QDialog::finished,
    // if the dialog closes while a test is still running.
    void stopAudioInputTest();

    QLineEdit *m_callsign;
    QLineEdit *m_suffix;
    QComboBox *m_moduleSuffix;
    QComboBox *m_audioInput;
    QPushButton *m_audioInputTest;
    QProgressBar *m_audioInputLevel;
    std::thread m_audioInputTestThread;
    std::atomic<bool> m_audioInputTesting{false};
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
