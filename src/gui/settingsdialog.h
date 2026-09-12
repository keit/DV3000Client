#pragma once

// Modal settings dialog: callsign, MYCALL2 suffix, our own module letter,
// audio in/out device, and the ThumbDV serial device. The device dropdowns
// are pre-populated by probing ALSA and /dev/serial/by-id (see .cpp) but
// stay editable, since not every valid device name shows up in either
// probe.

#include <QDialog>

#include "settings.h"

class QComboBox;
class QLineEdit;

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
    QComboBox *m_thumbdv;
};
