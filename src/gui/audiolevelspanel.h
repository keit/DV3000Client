#pragma once

// Mic and speaker volume sliders, shown at the bottom of both protocol
// tabs so they can be adjusted while communicating. Layout follows the
// author's pttUSB project (a "Microphone" and a "Speaker" group box, each
// a "Volume" label over a 0..100 slider), minus its test buttons -- the
// Settings dialog already has those. Purely the widget: the owning tab
// applies the values (see DStarTab/DmrTab::applyVolumes), and MainWindow
// keeps the two tabs' panels in sync and saves them.

#include <QWidget>

class QProgressBar;
class QSlider;

class AudioLevelsPanel : public QWidget {
    Q_OBJECT
public:
    explicit AudioLevelsPanel(QWidget *parent = nullptr);

    // Moves the sliders without emitting anything -- for syncing from the
    // other tab or from saved settings, where echoing the change back
    // would just loop.
    void setVolumes(int mic, int speaker);

    // Feeds the level meters (0..100 peaks, after gain) -- meant to be
    // called at a steady rate (the tabs poll every 50ms). VU-style: rises
    // instantly, falls a few points per call, so a brief peak is visible.
    void updateLevels(int mic, int speaker);

signals:
    // Every change, including while a slider is being dragged.
    void changed(int mic, int speaker);
    // A change has settled (slider released, or a keyboard/wheel step) --
    // the point at which to save, so dragging doesn't write the settings
    // file on every pixel.
    void committed();

private:
    QSlider *m_mic;
    QSlider *m_speaker;
    QProgressBar *m_micLevel;
    QProgressBar *m_speakerLevel;
};
