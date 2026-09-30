#pragma once

// Volume slider (0..100) to linear gain, and applying it to PCM. 50 =
// unity (so the default leaves audio untouched), 100 = +12 dB, 1 = -30 dB,
// 0 = mute. The extra headroom above unity is for quiet mics and small
// speakers. Same curve as the author's pttUSB project, so the sliders feel
// the same across both.

#include <cmath>
#include <cstddef>
#include <cstdint>

inline float sliderToGain(int v) {
    if (v <= 0) return 0.0f;
    float db = v <= 50 ? (v - 50) * 0.6f : (v - 50) * 0.24f;
    return std::pow(10.0f, db / 20.0f);
}

// Scales 16-bit samples in place, saturating rather than wrapping when the
// gain pushes a peak past full scale (wrapping would sound like a crack).
inline void applyGain(short *pcm, size_t count, float gain) {
    if (gain == 1.0f) return;
    for (size_t i = 0; i < count; i++) {
        float v = pcm[i] * gain;
        if (v > 32767.0f) v = 32767.0f;
        else if (v < -32768.0f) v = -32768.0f;
        pcm[i] = static_cast<short>(v);
    }
}

// Peak sample level on a 0..100 meter scale -- what the level meters show.
// Logarithmic, like a real audio meter: kMeterFloorDb dBFS (and below) is
// 0, full scale is 100. A linear scale barely moved for decoded speech,
// which typically peaks around 5-30% of full scale (-26..-10 dBFS) yet
// sounds fine; on this scale that's roughly 45-80.
inline int peakPercent(const short *pcm, size_t count) {
    constexpr float kMeterFloorDb = -48.0f;
    int peak = 0;
    for (size_t i = 0; i < count; i++) {
        int m = pcm[i] < 0 ? -static_cast<int>(pcm[i]) : pcm[i];
        if (m > peak) peak = m;
    }
    if (peak == 0) return 0;
    float db = 20.0f * std::log10(peak / 32767.0f);
    if (db <= kMeterFloorDb) return 0;
    if (db >= 0.0f) return 100;
    return static_cast<int>(100.0f * (1.0f - db / kMeterFloorDb));
}
