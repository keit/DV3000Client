#pragma once

// Modal settings dialog, split across two tabs: "General" (callsign,
// MYCALL2 suffix, our own module letter, and the DMR identity -- DMR ID,
// plus a server/password page per DMR network (BrandMeister or TGIF, only
// the chosen one shown) -- used by the
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
class QStackedWidget;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(const GuiSettings &current, QWidget *parent = nullptr);

    GuiSettings settings() const;

private:
    // What the dialog was opened with -- settings() starts from it so fields
    // the dialog doesn't edit (e.g. the tabs' volume sliders) aren't reset.
    GuiSettings m_initial;

    // Stops the input-level capture thread if running (sets the flag and
    // joins -- near-instant, since each ALSA read the thread does only
    // blocks for one ~20ms period) and resets the Test button/meter.
    // Called both when the user clicks Stop and, via QDialog::finished,
    // if the dialog closes while a test is still running.
    void stopAudioInputTest();

    // Shows the field page for the chosen DMR network -- called once at
    // construction and again on every change.
    void updateDmrNetworkPage();

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
    QComboBox *m_dmrNetwork;
    QStackedWidget *m_dmrNetworkPages;
    QLineEdit *m_bmServer;
    QLineEdit *m_bmPassword;
    QLineEdit *m_tgifServer;
    QLineEdit *m_tgifPassword;
    QLineEdit *m_dmrLatitude;
    QLineEdit *m_dmrLongitude;
    QLineEdit *m_dmrLocation;
    QLineEdit *m_dmrDescription;
    QLineEdit *m_dmrUrl;
};
