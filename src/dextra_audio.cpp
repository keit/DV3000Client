#include "dextra_audio.h"

#include <chrono>

namespace dextra {

void captureThread(audio::VocoderPipeline *vocoder, audio::AlsaPcm *capture, DextraClient *client,
                    std::function<bool()> pttActive) {
    bool transmitting = false;
    uint16_t streamId = 0;
    short pcm[SerialDV::MBE_AUDIO_BLOCK_SIZE];

    audio::CaptureThrottle throttle; // see alsa_audio.h

    auto endTransmission = [&] {
        vocoder->drain(); // the end-of-stream packet must follow every voice frame
        client->endLiveTx(streamId);
        transmitting = false;
    };

    while (g_running) {
        auto readStart = std::chrono::steady_clock::now();
        bool gotAudio = capture->read(pcm);
        auto readElapsed = std::chrono::steady_clock::now() - readStart;
        // Polled here, ahead of the throttle below, so the throttle knows
        // whether we're (about to be) transmitting this iteration.
        bool active = pttActive();

        throttle.afterRead(readElapsed, active);

        if (!gotAudio) continue;

        if (active && !transmitting) {
            streamId = client->beginLiveTx();
            transmitting = true;
        } else if (!active && transmitting) {
            endTransmission();
        }

        if (transmitting) {
            uint16_t id = streamId;
            vocoder->encode(pcm, [client, id](const uint8_t *ambe) { client->sendLiveTxFrame(id, ambe); });
        }
    }

    if (transmitting) endTransmission();
}

} // namespace dextra
