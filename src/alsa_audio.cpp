#include "alsa_audio.h"

#include "audio_gain.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>

namespace audio {

bool AlsaPcm::open(const std::string &device, snd_pcm_stream_t stream) {
    m_stream = stream;
    int err = snd_pcm_open(&m_handle, device.c_str(), stream, 0);
    if (err < 0) {
        std::fprintf(stderr, "audio: snd_pcm_open(%s) failed: %s\n",
                     device.c_str(), snd_strerror(err));
        return false;
    }

    unsigned int rate = 8000;
    // Buffer time in microseconds -- 40 periods of one DV frame (20ms)
    // each, ~800ms total. Previously 20 periods (~400ms, itself raised from
    // an original 4/~80ms for the same reason below), which was enough
    // margin for a local serial ThumbDV's occasional slow-but-live retry,
    // but not for a ThumbDV reached over the network (see udpdatacontroller
    // in the serialDV fork): a per-frame round trip there normally costs
    // ~15-20ms already, so any scheduling delay against the GUI event loop,
    // network thread, and capture/playback threads all sharing the same box
    // eats into a much thinner margin than the same delay would against a
    // ~1-2ms local serial exchange, and drains the buffer into an underrun
    // (EPIPE/"Broken Pipe") within a couple of seconds -- confirmed against
    // a real AMBEServer 3000 on a LAN, dropping whole words of audio.
    // Doubling it covers that without the local-serial path losing anything:
    // a few hundred ms to a second of extra latency is inaudible for a
    // half-duplex PTT voice app; audible dropouts aren't.
    err = snd_pcm_set_params(m_handle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                              1, rate, 1, 40 * (1000000 / (rate / SerialDV::MBE_AUDIO_BLOCK_SIZE)));
    if (err < 0) {
        std::fprintf(stderr, "audio: snd_pcm_set_params(%s) failed: %s\n",
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

void AlsaPcm::drain() {
    if (m_handle) snd_pcm_drain(m_handle);
}

bool AlsaPcm::read(short *pcm) {
    snd_pcm_sframes_t n = snd_pcm_readi(m_handle, pcm, SerialDV::MBE_AUDIO_BLOCK_SIZE);
    if (n == static_cast<snd_pcm_sframes_t>(SerialDV::MBE_AUDIO_BLOCK_SIZE)) {
        applyGain(pcm, SerialDV::MBE_AUDIO_BLOCK_SIZE, m_gain.load());
        notePeak(pcm);
        return true;
    }
    if (n < 0) return recover(static_cast<int>(n));
    return false;
}

bool AlsaPcm::write(const short *pcm) {
    // Scaled into a copy -- pcm is const, and callers (the playback
    // thread's silence backfill, a queue chunk) may reuse it.
    short scaled[SerialDV::MBE_AUDIO_BLOCK_SIZE];
    std::copy(pcm, pcm + SerialDV::MBE_AUDIO_BLOCK_SIZE, scaled);
    applyGain(scaled, SerialDV::MBE_AUDIO_BLOCK_SIZE, m_gain.load());
    notePeak(scaled);
    snd_pcm_sframes_t n = snd_pcm_writei(m_handle, scaled, SerialDV::MBE_AUDIO_BLOCK_SIZE);
    if (n == static_cast<snd_pcm_sframes_t>(SerialDV::MBE_AUDIO_BLOCK_SIZE)) return true;
    if (n < 0) return recover(static_cast<int>(n));
    return false;
}

void AlsaPcm::notePeak(const short *pcm) {
    int p = peakPercent(pcm, SerialDV::MBE_AUDIO_BLOCK_SIZE);
    int cur = m_peak.load();
    while (p > cur && !m_peak.compare_exchange_weak(cur, p)) {
    }
}

bool AlsaPcm::recover(int err) {
    std::fprintf(stderr, "audio: ALSA %s xrun/error: %s\n",
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

void playbackThread(AlsaPcm *playback, PcmQueue *queue, const volatile std::sig_atomic_t *running) {
    short pcm[SerialDV::MBE_AUDIO_BLOCK_SIZE];
    // Start "idle": nothing's been heard yet, so there's nothing to backfill.
    auto lastRealFrame = std::chrono::steady_clock::now() - std::chrono::seconds(10);

    while (*running) {
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

void CaptureThrottle::afterRead(std::chrono::steady_clock::duration readElapsed, bool transmitting) {
    constexpr auto period = std::chrono::milliseconds(20);
    constexpr auto fastThreshold = std::chrono::milliseconds(2);
    constexpr int fastStreakLimit = 5;

    if (readElapsed < fastThreshold) {
        if (m_fastReadStreak < fastStreakLimit) m_fastReadStreak++;
    } else {
        m_fastReadStreak = 0;
    }
    if (m_fastReadStreak >= fastStreakLimit && !transmitting) std::this_thread::sleep_for(period);
}

} // namespace audio
