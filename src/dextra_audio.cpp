#include "dextra_audio.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <mutex>

namespace dextra {

void captureThread(SerialDV::DVController *dv, audio::AlsaPcm *capture, DextraClient *client,
                    std::function<bool()> pttActive) {
    bool transmitting = false;
    uint16_t streamId = 0;
    short pcm[SerialDV::MBE_AUDIO_BLOCK_SIZE];

    audio::CaptureThrottle throttle; // see alsa_audio.h

    while (g_running) {
        auto readStart = std::chrono::steady_clock::now();
        bool gotAudio = capture->read(pcm);
        auto readElapsed = std::chrono::steady_clock::now() - readStart;
        // Polled here, ahead of the throttle below, so the throttle knows
        // whether we're (about to be) transmitting this iteration.
        bool active = pttActive();

        throttle.afterRead(readElapsed, active);

        if (gotAudio) {
            if (active && !transmitting) {
                streamId = client->beginLiveTx();
                transmitting = true;
            } else if (!active && transmitting) {
                client->endLiveTx(streamId);
                transmitting = false;
            }

            if (transmitting) {
                std::array<uint8_t, AMBE_SIZE> ambe{};
                bool ok;
                {
                    std::lock_guard<std::mutex> lock(g_dvMutex);
                    ok = dv->encode(pcm, ambe.data(), SerialDV::DVRate3600x2400);
                }
                if (ok) {
                    client->sendLiveTxFrame(streamId, ambe.data());
                } else {
                    std::fprintf(stderr, "dextra_audio: AMBE encode failed, dropping frame\n");
                }
            }
        }
    }

    if (transmitting) client->endLiveTx(streamId);
}

} // namespace dextra
