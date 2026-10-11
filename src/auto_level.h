#pragma once

// Automatic level for received audio: stations arrive at very different
// loudness, so this brings each towards a common speech level before the
// speaker volume is applied. Runs on the playback thread, one 20 ms frame
// at a time; switched on and off from the GUI thread.
//
// - Loud speech is turned down at once; quiet speech is brought up
//   gradually (about 12 dB a second), so syllables don't pump.
// - Frames below a speech threshold (pauses, background) hold the gain
//   rather than raising it, so noise isn't brought up between words.
// - Gain stays within +/-12 dB, and a limiter keeps any frame from
//   clipping.
// - reset() at the start of each over, so one station's level doesn't
//   carry over to the next.
// - Gain changes are ramped across each frame, so they never click.

#include <atomic>
#include <cstddef>

namespace audio {

class AutoLevel {
public:
    void setEnabled(bool enabled) { m_enabled.store(enabled); }
    bool enabled() const { return m_enabled.load(); }

    // A new over is starting: back to unity gain, no level measured yet.
    void reset();

    // Levels count samples in place (no-op while disabled).
    void process(short *pcm, size_t count);

private:
    std::atomic<bool> m_enabled{true};
    float m_gain = 1.0f;
    float m_envelope = 0.0f; // recent speech level (RMS), 0 = none heard yet this over
};

} // namespace audio
