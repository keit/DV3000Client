#include "dmr_audio.h"

#include <chrono>
#include <cstdio>
#include <cstring>

namespace dmr {

namespace {

// One 60ms voice burst being assembled from encode replies. Filled in on the
// vocoder's reply thread; the capture thread only looks at it after
// VocoderPipeline::drain(), which orders the two.
struct Burst {
    DmrTransport *client = nullptr;
    uint32_t streamId = 0;
    uint8_t ambe[3][AMBE_FRAME_SIZE];
    int count = 0;

    void add(const uint8_t *frame) {
        std::memcpy(ambe[count++], frame, AMBE_FRAME_SIZE);
        if (count == 3) {
            client->sendVoiceFrame(streamId, ambe[0], ambe[1], ambe[2]);
            count = 0;
        }
    }
};

} // namespace

void captureThread(audio::VocoderPipeline *vocoder, audio::AlsaPcm *capture, DmrTransport *client,
                    std::function<uint32_t()> talkgroup, std::function<dmr::CallType()> callType,
                    std::function<bool()> pttActive) {
    bool transmitting = false;
    Burst burst;
    burst.client = client;
    short pcm[SerialDV::MBE_AUDIO_BLOCK_SIZE];

    audio::CaptureThrottle throttle; // see alsa_audio.h

    auto encodeInto = [&](const short *frame) {
        vocoder->encode(frame, [&burst](const uint8_t *ambe) { burst.add(ambe); });
    };

    auto endTransmission = [&] {
        vocoder->drain(); // every voice frame so far is back and sent
        // A partial burst (PTT released mid-way through its 3 frames)
        // still needs to go out complete -- pad the remainder with
        // silence rather than sending a short/malformed packet.
        if (burst.count > 0) {
            static const short silence[SerialDV::MBE_AUDIO_BLOCK_SIZE] = {};
            for (int i = burst.count; i < 3; i++) encodeInto(silence);
            vocoder->drain();
            burst.count = 0; // a padding frame that failed: drop the partial burst
        }
        client->endVoiceTx(burst.streamId);
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
            burst.streamId = client->beginVoiceTx(talkgroup(), callType());
            burst.count = 0;
            transmitting = true;
        } else if (!active && transmitting) {
            endTransmission();
        }

        if (transmitting) encodeInto(pcm);
    }

    if (transmitting) endTransmission();
}

std::function<void(const uint8_t *, const uint8_t *, const uint8_t *)>
makeVoiceRxHandler(audio::VocoderPipeline *vocoder, audio::PcmQueue *queue) {
    return [vocoder, queue](const uint8_t *ambe0, const uint8_t *ambe1, const uint8_t *ambe2) {
        for (const uint8_t *ambe : {ambe0, ambe1, ambe2})
            vocoder->decode(ambe, [queue](const short *pcm) { queue->push(pcm); });
    };
}

} // namespace dmr
