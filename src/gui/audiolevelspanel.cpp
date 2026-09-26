#include "audiolevelspanel.h"

#include <QGroupBox>
#include <QLabel>
#include <QProgressBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>

#include <cmath>

#include "audio_gain.h"

namespace {
// 50 = unity, so say what the number means in dB.
QString volumeTooltip(const QString &what, int v) {
    float gain = sliderToGain(v);
    if (gain <= 0.0f) return what + ": muted";
    return QString("%1: %2 dB").arg(what).arg(20.0 * std::log10(gain), 0, 'f', 1);
}

// Same green/amber/red look as the Settings dialog's input meter, with a
// darker copy of the gradient as the empty track so the whole scale is
// visible even at low levels.
QProgressBar *makeMeter() {
    auto *bar = new QProgressBar;
    bar->setRange(0, 100);
    bar->setTextVisible(false);
    bar->setFixedHeight(10);
    bar->setStyleSheet(
        "QProgressBar { border: 1px solid gray; background: qlineargradient(x1:0, y1:0, x2:1, y2:0,"
        "  stop:0 #2e6b32, stop:0.7 #2e6b32, stop:0.85 #8a5a00, stop:1 #8a2424); }"
        "QProgressBar::chunk { background: qlineargradient(x1:0, y1:0, x2:1, y2:0,"
        "  stop:0 #4CAF50, stop:0.7 #4CAF50, stop:0.85 #ff9800, stop:1 #f44336); }");
    return bar;
}

QSlider *makeSlider(QGroupBox *box, const QString &what, QProgressBar *meter) {
    auto *slider = new QSlider(Qt::Horizontal);
    slider->setRange(0, 100);
    slider->setValue(50);
    slider->setToolTip(volumeTooltip(what, 50));
    QObject::connect(slider, &QSlider::valueChanged, slider, [slider, what](int v) { slider->setToolTip(volumeTooltip(what, v)); });

    auto *layout = new QVBoxLayout(box);
    layout->setContentsMargins(8, 4, 8, 6);
    layout->setSpacing(2);
    layout->addWidget(new QLabel("Volume"));
    layout->addWidget(slider);
    layout->addWidget(meter);
    return slider;
}
} // namespace

AudioLevelsPanel::AudioLevelsPanel(QWidget *parent) : QWidget(parent) {
    auto *micBox = new QGroupBox("Microphone");
    m_micLevel = makeMeter();
    m_mic = makeSlider(micBox, "Microphone", m_micLevel);
    auto *speakerBox = new QGroupBox("Speaker");
    m_speakerLevel = makeMeter();
    m_speaker = makeSlider(speakerBox, "Speaker", m_speakerLevel);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(micBox);
    layout->addWidget(speakerBox);

    for (QSlider *slider : {m_mic, m_speaker}) {
        connect(slider, &QSlider::valueChanged, this, [this, slider](int) {
            emit changed(m_mic->value(), m_speaker->value());
            if (!slider->isSliderDown()) emit committed(); // keyboard/wheel step
        });
        connect(slider, &QSlider::sliderReleased, this, &AudioLevelsPanel::committed);
    }
}

void AudioLevelsPanel::setVolumes(int mic, int speaker) {
    // Blocked so the other tab's echo doesn't bounce back -- which also
    // blocks the tooltip update that hangs off valueChanged, hence the
    // explicit one.
    QSignalBlocker blockMic(m_mic), blockSpeaker(m_speaker);
    m_mic->setValue(mic);
    m_speaker->setValue(speaker);
    m_mic->setToolTip(volumeTooltip("Microphone", mic));
    m_speaker->setToolTip(volumeTooltip("Speaker", speaker));
}

void AudioLevelsPanel::updateLevels(int mic, int speaker) {
    constexpr int kFall = 4; // points per call -- ~200/s at the tabs' 50ms poll
    m_micLevel->setValue(qMax(mic, m_micLevel->value() - kFall));
    m_speakerLevel->setValue(qMax(speaker, m_speakerLevel->value() - kFall));
}
