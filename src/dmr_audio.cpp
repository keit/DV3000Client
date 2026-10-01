#include "dmr_audio.h"

#include <chrono>
#include <cstdio>

namespace dmr {

std::mutex g_dvMutex;

namespace {

// Encodes one 20ms silence frame -- used to pad out a burst that PTT
// released partway through, since DMR always sends complete 60ms bursts
// (unlike D-Star, which can send a lone final AMBE frame).
void encodeSilence(SerialDV::DVController *dv, uint8_t ambe[AMBE_FRAME_SIZE]) {
    short silence[SerialDV::MBE_AUDIO_BLOCK_SIZE] = {};
    std::lock_guard<std::mutex> lock(g_dvMutex);
    dv->encode(silence, ambe, SerialDV::DVRate3600x2450);
}

} // namespace

void captureThread(SerialDV::DVController *dv, audio::AlsaPcm *capture, DmrTransport *client,
                    std::function<uint32_t()> talkgroup, std::function<dmr::CallType()> callType,
                    std::function<bool()> pttActive) {
    bool transmitting = false;
    uint32_t streamId = 0;
    int frameIndex = 0; // which of this burst's 3 AMBE frames comes next
    uint8_t ambe[3][AMBE_FRAME_SIZE];
    short pcm[SerialDV::MBE_AUDIO_BLOCK_SIZE];

    audio::CaptureThrottle throttle; // see alsa_audio.h

    auto endTransmission = [&] {
        // A partial burst (PTT released mid-way through its 3 frames)
        // still needs to go out complete -- pad the remainder with
        // silence rather than sending a short/malformed packet.
        if (frameIndex > 0) {
            while (frameIndex < 3) {
                encodeSilence(dv, ambe[frameIndex]);
                frameIndex++;
            }
            client->sendVoiceFrame(streamId, ambe[0], ambe[1], ambe[2]);
            frameIndex = 0;
        }
        client->endVoiceTx(streamId);
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
            streamId = client->beginVoiceTx(talkgroup(), callType());
            transmitting = true;
            frameIndex = 0;
        } else if (!active && transmitting) {
            endTransmission();
        }

        if (!transmitting) continue;

        {
            std::lock_guard<std::mutex> lock(g_dvMutex);
            if (!dv->encode(pcm, ambe[frameIndex], SerialDV::DVRate3600x2450)) {
                std::fprintf(stderr, "dmr_audio: AMBE encode failed, dropping frame\n");
                continue;
            }
        }
        frameIndex++;
        if (frameIndex == 3) {
            client->sendVoiceFrame(streamId, ambe[0], ambe[1], ambe[2]);
            frameIndex = 0;
        }
    }

    if (transmitting) endTransmission();
}

std::function<void(const uint8_t *, const uint8_t *, const uint8_t *)>
makeVoiceRxHandler(SerialDV::DVController *dv, audio::PcmQueue *queue) {
    return [dv, queue](const uint8_t *ambe0, const uint8_t *ambe1, const uint8_t *ambe2) {
        for (const uint8_t *ambe : {ambe0, ambe1, ambe2}) {
            short pcm[SerialDV::MBE_AUDIO_BLOCK_SIZE];
            bool ok;
            {
                std::lock_guard<std::mutex> lock(g_dvMutex);
                ok = dv->decode(pcm, ambe, SerialDV::DVRate3600x2450);
            }
            if (ok) queue->push(pcm);
        }
    };
}

} // namespace dmr
