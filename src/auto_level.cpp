#include "auto_level.h"

#include <algorithm>
#include <cmath>

namespace audio {

namespace {
constexpr float kTargetRms = 3000.0f;  // ~ -21 dBFS: comfortable speech, room left for peaks
constexpr float kSpeechRms = 150.0f;   // ~ -47 dBFS: quieter than this is a pause, not speech
constexpr float kMinGain = 0.25f;      // -12 dB
constexpr float kMaxGain = 4.0f;       // +12 dB
constexpr float kEnvelopeDecay = 0.98f;                // per 20 ms frame: ~1 s to settle after a loud word
const float kRisePerFrame = std::pow(10.0f, 0.25f / 20); // +0.25 dB per frame, ~12 dB/s
constexpr float kPeakLimit = 30000.0f; // just under full scale
} // namespace

void AutoLevel::reset() {
    m_gain = 1.0f;
    m_envelope = 0.0f;
}

void AutoLevel::process(short *pcm, size_t count) {
    if (!m_enabled.load() || count == 0) return;

    double sumSquares = 0;
    float peak = 0;
    for (size_t i = 0; i < count; i++) {
        sumSquares += static_cast<double>(pcm[i]) * pcm[i];
        peak = std::max(peak, std::fabs(static_cast<float>(pcm[i])));
    }
    float rms = static_cast<float>(std::sqrt(sumSquares / count));

    // The envelope follows speech only: up at once, down slowly; held
    // through pauses.
    if (rms >= kSpeechRms) m_envelope = std::max(rms, m_envelope * kEnvelopeDecay);

    float wanted = m_envelope > 0 ? std::clamp(kTargetRms / m_envelope, kMinGain, kMaxGain) : m_gain;
    float next = wanted < m_gain ? wanted : std::min(wanted, m_gain * kRisePerFrame);
    // Never push this frame's peak into clipping.
    if (peak * next > kPeakLimit) next = kPeakLimit / peak;

    // Ramp from the previous frame's gain to this one's across the frame.
    for (size_t i = 0; i < count; i++) {
        float g = m_gain + (next - m_gain) * static_cast<float>(i + 1) / count;
        float v = pcm[i] * g;
        pcm[i] = static_cast<short>(std::clamp(v, -32768.0f, 32767.0f));
    }
    m_gain = next;
}

} // namespace audio
