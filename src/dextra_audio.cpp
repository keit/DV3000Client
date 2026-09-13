#include "dextra_audio.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace dextra {

bool AlsaPcm::open(const std::string &device, snd_pcm_stream_t stream) {
    m_stream = stream;
    int err = snd_pcm_open(&m_handle, device.c_str(), stream, 0);
    if (err < 0) {
        std::fprintf(stderr, "dextra_audio: snd_pcm_open(%s) failed: %s\n",
                     device.c_str(), snd_strerror(err));
        return false;
    }

    unsigned int rate = 8000;
    // Buffer time in microseconds -- 20 periods of one DV frame (20ms)
    // each, ~400ms total. Previously 4 periods (~80ms), which left almost
    // no slack: any brief scheduling delay against the GUI event loop,
    // network thread, and capture thread all sharing the same box -- or a
    // slow-but-still-live decode retry (see getResponse()'s progress-based
    // retry in the serialDV fork, which deliberately keeps going rather
    // than truncating a slow transfer) -- could drain it and underrun
    // (EPIPE/"Broken Pipe"). A few hundred ms of extra latency is
    // inaudible for a half-duplex PTT voice app; audible dropouts aren't.
    err = snd_pcm_set_params(m_handle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                              1, rate, 1, 20 * (1000000 / (rate / SerialDV::MBE_AUDIO_BLOCK_SIZE)));
    if (err < 0) {
        std::fprintf(stderr, "dextra_audio: snd_pcm_set_params(%s) failed: %s\n",
                     device.c_str(), snd_strerror(err));
        snd_pcm_close(m_handle);
        m_handle = nullptr;
        return false;
    }
    return true;
}

void AlsaPcm::close() {
    if (m_handle) {
        snd_pcm_close(m_handle);
        m_handle = nullptr;
    }
}

bool AlsaPcm::read(short *pcm) {
    snd_pcm_sframes_t n = snd_pcm_readi(m_handle, pcm, SerialDV::MBE_AUDIO_BLOCK_SIZE);
    if (n == static_cast<snd_pcm_sframes_t>(SerialDV::MBE_AUDIO_BLOCK_SIZE)) return true;
    if (n < 0) return recover(static_cast<int>(n));
    return false;
}

bool AlsaPcm::write(const short *pcm) {
    snd_pcm_sframes_t n = snd_pcm_writei(m_handle, pcm, SerialDV::MBE_AUDIO_BLOCK_SIZE);
    if (n == static_cast<snd_pcm_sframes_t>(SerialDV::MBE_AUDIO_BLOCK_SIZE)) return true;
    if (n < 0) return recover(static_cast<int>(n));
    return false;
}

bool AlsaPcm::recover(int err) {
    std::fprintf(stderr, "dextra_audio: ALSA %s xrun/error: %s\n",
                 m_stream == SND_PCM_STREAM_CAPTURE ? "capture" : "playback", snd_strerror(err));
    return snd_pcm_recover(m_handle, err, 1) == 0;
}

void PcmQueue::push(const short *pcm) {
    std::array<short, SerialDV::MBE_AUDIO_BLOCK_SIZE> chunk;
    std::memcpy(chunk.data(), pcm, sizeof(chunk));
    std::lock_guard<std::mutex> lock(m_mutex);
    // Drop the oldest chunk rather than growing unbounded if playback ever
    // falls behind -- a little audio loss beats unbounded latency.
    if (m_queue.size() > 50) m_queue.pop_front();
    m_queue.push_back(chunk);
    m_cv.notify_one();
}

bool PcmQueue::pop(short *pcm, int timeoutMs) {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return !m_queue.empty(); })) {
        return false;
    }
    std::memcpy(pcm, m_queue.front().data(), sizeof(short) * SerialDV::MBE_AUDIO_BLOCK_SIZE);
    m_queue.pop_front();
    return true;
}

void captureThread(SerialDV::DVController *dv, AlsaPcm *capture, DextraClient *client,
                    std::function<bool()> pttActive) {
    bool transmitting = false;
    uint16_t streamId = 0;
    short pcm[SerialDV::MBE_AUDIO_BLOCK_SIZE];

    // One DV frame period. AlsaPcm::read() is assumed to block for roughly
    // this long on a real device, which is what paces this loop -- but a
    // stand-in device (e.g. ALSA's "null", used when no mic is attached yet;
    // confirmed via `arecord -D null -d 3` returning in ~4ms instead of 3s)
    // returns instantly instead of blocking, spinning this thread at 100%
    // CPU with no pacing of its own.
    //
    // Padding every iteration up to a fixed period would fix that but break
    // real hardware: a device catching up after a brief timing hiccup (e.g.
    // a wireless dongle) legitimately returns faster than one period for a
    // call or two while draining backlog, and forcing a sleep there would
    // just prevent that catch-up, growing the backlog until the capture
    // ring buffer overruns instead. So only throttle once several
    // *consecutive* reads return near-instantly -- real hardware never
    // sustains that under normal jitter, only a genuinely non-blocking
    // device does.
    constexpr auto period = std::chrono::milliseconds(20);
    constexpr auto fastThreshold = std::chrono::milliseconds(2);
    constexpr int fastStreakLimit = 5;
    int fastReadStreak = 0;

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

        if (gotAudio) {
            bool active = pttActive();
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

void playbackThread(AlsaPcm *playback, PcmQueue *queue) {
    short pcm[SerialDV::MBE_AUDIO_BLOCK_SIZE];
    // Start "idle": nothing's been heard yet, so there's nothing to backfill.
    auto lastRealFrame = std::chrono::steady_clock::now() - std::chrono::seconds(10);

    while (g_running) {
        if (queue->pop(pcm, 20)) {
            lastRealFrame = std::chrono::steady_clock::now();
            playback->write(pcm);
            continue;
        }

        // No real frame within this ~20ms slot. Only backfill with silence
        // if we were receiving real audio recently (a brief within-
        // transmission gap -- real-world network delivery isn't perfectly
        // isochronous, and a source fractionally slower than local
        // playback drains the queue on a roughly periodic cycle during
        // long transmissions specifically; confirmed in the field as
        // "Broken pipe" xruns spaced ~1.5-3s apart through a 20+ second
        // transmission). Once we've genuinely gone quiet for a while,
        // stop touching the device entirely rather than writing silence
        // forever -- an earlier version of this did that unconditionally,
        // and it turned out to matter: something (PipeWire/ALSA
        // auto-suspending an idle sink, most likely) was already
        // periodically suspending this device during real silence, and
        // continuously probing it just kept rediscovering that as a
        // "Broken pipe" every ~30s, all night, instead of the rare
        // mid-transmission case this is actually meant to cover.
        if (std::chrono::steady_clock::now() - lastRealFrame < std::chrono::milliseconds(500)) {
            std::memset(pcm, 0, sizeof(pcm));
            playback->write(pcm);
        }
    }
}

} // namespace dextra
