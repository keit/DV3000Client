#include "dmr_audio.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

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

void captureThread(SerialDV::DVController *dv, dextra::AlsaPcm *capture, DmrClient *client,
                    std::function<uint32_t()> talkgroup, std::function<bool()> pttActive) {
    bool transmitting = false;
    uint32_t streamId = 0;
    int frameInBurst = 0;
    int frameIndex = 0; // which of this burst's 3 AMBE frames comes next
    uint8_t ambe[3][AMBE_FRAME_SIZE];
    short pcm[SerialDV::MBE_AUDIO_BLOCK_SIZE];

    // Same fast-read throttle as dextra::captureThread -- see its comments
    // for why this only kicks in after several consecutive near-instant
    // reads rather than padding every iteration unconditionally.
    constexpr auto period = std::chrono::milliseconds(20);
    constexpr auto fastThreshold = std::chrono::milliseconds(2);
    constexpr int fastStreakLimit = 5;
    int fastReadStreak = 0;

    auto endTransmission = [&] {
        // A partial burst (PTT released mid-way through its 3 frames)
        // still needs to go out complete -- pad the remainder with
        // silence rather than sending a short/malformed packet.
        if (frameIndex > 0) {
            while (frameIndex < 3) {
                encodeSilence(dv, ambe[frameIndex]);
                frameIndex++;
            }
            client->sendVoiceFrame(streamId, frameInBurst, ambe[0], ambe[1], ambe[2]);
            frameIndex = 0;
        }
        client->endVoiceTx(streamId);
        transmitting = false;
    };

    while (g_running) {
        auto readStart = std::chrono::steady_clock::now();
        bool gotAudio = capture->read(pcm);
        auto readElapsed = std::chrono::steady_clock::now() - readStart;

        if (readElapsed < fastThreshold) {
            if (fastReadStreak < fastStreakLimit) fastReadStreak++;
        } else {
            fastReadStreak = 0;
        }
        if (fastReadStreak >= fastStreakLimit) {
            std::this_thread::sleep_for(period);
        }

        if (!gotAudio) continue;

        bool active = pttActive();
        if (active && !transmitting) {
            streamId = client->beginVoiceTx(talkgroup());
            transmitting = true;
            frameInBurst = 0;
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
            client->sendVoiceFrame(streamId, frameInBurst, ambe[0], ambe[1], ambe[2]);
            frameIndex = 0;
            frameInBurst = (frameInBurst + 1) % 6;
        }
    }

    if (transmitting) endTransmission();
}

std::function<void(const uint8_t *, const uint8_t *, const uint8_t *)>
makeVoiceRxHandler(SerialDV::DVController *dv, dextra::PcmQueue *queue) {
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

void playbackThread(dextra::AlsaPcm *playback, dextra::PcmQueue *queue) {
    short pcm[SerialDV::MBE_AUDIO_BLOCK_SIZE];
    auto lastRealFrame = std::chrono::steady_clock::now() - std::chrono::seconds(10);

    while (g_running) {
        if (queue->pop(pcm, 20)) {
            lastRealFrame = std::chrono::steady_clock::now();
            playback->write(pcm);
            continue;
        }
        if (std::chrono::steady_clock::now() - lastRealFrame < std::chrono::milliseconds(500)) {
            std::memset(pcm, 0, sizeof(pcm));
            playback->write(pcm);
        }
    }
}

} // namespace dmr
