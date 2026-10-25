#include "ysf_audio.h"

#include <chrono>

namespace ysf {

volatile std::sig_atomic_t g_running = 1;

void captureThread(audio::VocoderPipeline *vocoder, audio::AlsaPcm *capture, YsfClient *client,
                   const uint8_t *silence, std::function<bool()> pttActive) {
    bool transmitting = false;
    short pcm[SerialDV::MBE_AUDIO_BLOCK_SIZE];
    audio::CaptureThrottle throttle; // see alsa_audio.h

    auto endTransmission = [&] {
        vocoder->drain(); // the terminator must follow every voice frame
        client->endTx(silence);
        transmitting = false;
    };

    while (g_running) {
        auto readStart = std::chrono::steady_clock::now();
        bool gotAudio = capture->read(pcm);
        auto readElapsed = std::chrono::steady_clock::now() - readStart;
        bool active = pttActive();
        throttle.afterRead(readElapsed, active);
        if (!gotAudio) continue;

        if (active && !transmitting) {
            client->beginTx();
            transmitting = true;
        } else if (!active && transmitting) {
            endTransmission();
        }
        if (transmitting) vocoder->encode(pcm, [client](const uint8_t *ambe) { client->sendVoice(ambe); });
    }
    if (transmitting) endTransmission();
}

} // namespace ysf
