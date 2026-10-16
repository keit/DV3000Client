#include "alsa_audio.h"

#include "audio_gain.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>

namespace audio {

namespace {
// Frames (20 ms each) queued before playback starts -- see AlsaPcm::open().
// 120 ms: the speaker volume and every over are heard that long after
// they're decoded, instead of 800 ms.
constexpr unsigned int kPlaybackStartFrames = 6;
} // namespace

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
    unsigned int bufferTime = 40 * (1000000 / (rate / SerialDV::MBE_AUDIO_BLOCK_SIZE));
    err = setHwParams(rate, bufferTime);
    if (err < 0)
        err = snd_pcm_set_params(m_handle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1, rate, 1,
                                 bufferTime);
    if (err < 0) {
        std::fprintf(stderr, "audio: snd_pcm_set_params(%s) failed: %s\n",
                     device.c_str(), snd_strerror(err));
        snd_pcm_close(m_handle);
        m_handle = nullptr;
        return false;
    }

    snd_pcm_uframes_t periodSize = 0;
    if (snd_pcm_get_params(m_handle, &m_bufferSize, &periodSize) < 0) m_bufferSize = 0;

    if (stream == SND_PCM_STREAM_PLAYBACK) {
        // snd_pcm_set_params() only starts playback once the whole buffer
        // is full, so every over was heard 800 ms late -- and the speaker
        // volume, applied as frames go in (write()), lagged the slider by
        // as much. Start once kPlaybackStartFrames are queued instead: the
        // rest of the buffer is still there to absorb bursts. Enough now
        // that requests to the ThumbDV are pipelined (vocoder_pipeline.h),
        // so decoded audio no longer arrives in fits and starts.
        snd_pcm_sw_params_t *sw;
        snd_pcm_sw_params_alloca(&sw);
        if (snd_pcm_sw_params_current(m_handle, sw) == 0 &&
            snd_pcm_sw_params_set_start_threshold(m_handle, sw,
                                                  kPlaybackStartFrames * SerialDV::MBE_AUDIO_BLOCK_SIZE) == 0) {
            err = snd_pcm_sw_params(m_handle, sw);
        }
        if (err < 0) {
            std::fprintf(stderr, "audio: cannot lower playback start threshold on %s: %s\n", device.c_str(),
                         snd_strerror(err));
        }
    }
    return true;
}

int AlsaPcm::setHwParams(unsigned int rate, unsigned int bufferTime) {
    // As snd_pcm_set_params() would, but with 20 ms periods (one DV frame)
    // rather than its buffer/4 = 200 ms. Through plughw's rate converter
    // (any card that can't do 8 kHz itself, e.g. a 48 kHz-only USB
    // headset or microphone) audio moves a period at a time:
    // - playback: with only a few frames queued -- playback starts at
    //   120 ms, and frames arrive just in time -- a 200 ms period kept
    //   running dry: an xrun every ~350 ms (seen on a Pi with a JBL
    //   headset; 0 xruns with 20 ms periods);
    // - capture: reads returned 10 frames at once every 200 ms, so the
    //   capture thread sent the ThumbDV 10 encode requests at a time --
    //   over the pipeline's limit of 8, and through an AMBEServer (~13 ms
    //   each) the last ones waited past the 100 ms reply timeout:
    //   "cannot get response", "dropping transmit frames". With 20 ms
    //   periods reads come every 20 ms.
    // Cards that do 8 kHz natively never showed either.
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    unsigned int periodTime = 1000000 / (rate / SerialDV::MBE_AUDIO_BLOCK_SIZE);
    int err = snd_pcm_hw_params_any(m_handle, hw);
    if (err >= 0) err = snd_pcm_hw_params_set_rate_resample(m_handle, hw, 1);
    if (err >= 0) err = snd_pcm_hw_params_set_access(m_handle, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err >= 0) err = snd_pcm_hw_params_set_format(m_handle, hw, SND_PCM_FORMAT_S16_LE);
    if (err >= 0) err = snd_pcm_hw_params_set_channels(m_handle, hw, 1);
    if (err >= 0) err = snd_pcm_hw_params_set_rate(m_handle, hw, rate, 0);
    if (err >= 0) err = snd_pcm_hw_params_set_period_time_near(m_handle, hw, &periodTime, nullptr);
    if (err >= 0) err = snd_pcm_hw_params_set_buffer_time_near(m_handle, hw, &bufferTime, nullptr);
    if (err >= 0) err = snd_pcm_hw_params(m_handle, hw);
    if (err < 0) {
        std::fprintf(stderr, "audio: 20 ms %s periods not available (%s), using the defaults\n",
                     m_stream == SND_PCM_STREAM_PLAYBACK ? "playback" : "capture", snd_strerror(err));
    }
    return err;
}

bool AlsaPcm::runningLow(long samples) {
    if (!m_handle || m_bufferSize == 0 || snd_pcm_state(m_handle) != SND_PCM_STATE_RUNNING) return false;
    // How much of *our* buffer is still unplayed: what an underrun depends
    // on. Not snd_pcm_delay(), which through PipeWire's ALSA plugin (the
    // "default" device on Raspberry Pi OS) also counts audio already
    // handed to the server -- PipeWire pulls in big chunks, so our buffer
    // ran dry while the delay still looked healthy: an xrun every few
    // hundred ms.
    snd_pcm_sframes_t avail = snd_pcm_avail(m_handle);
    if (avail < 0) return true; // already in trouble: let write() recover it
    long queued = static_cast<long>(m_bufferSize) - static_cast<long>(avail);
    return queued < samples;
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
    if (n < 0 && recover(static_cast<int>(n))) {
        // The frame that hit the underrun still gets played: dropping it
        // left an audible blip in the middle of the audio.
        n = snd_pcm_writei(m_handle, scaled, SerialDV::MBE_AUDIO_BLOCK_SIZE);
    }
    return n == static_cast<snd_pcm_sframes_t>(SerialDV::MBE_AUDIO_BLOCK_SIZE);
}

void AlsaPcm::restartIfStopped() {
    if (!m_handle) return;
    snd_pcm_state_t state = snd_pcm_state(m_handle);
    // Expected after a pause, so silent (recover's last argument), unlike a
    // real underrun mid-over, which write() logs.
    if (state == SND_PCM_STATE_XRUN) snd_pcm_recover(m_handle, -EPIPE, 1);
    else if (state == SND_PCM_STATE_SUSPENDED) snd_pcm_recover(m_handle, -ESTRPIPE, 1);
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

void playbackThread(AlsaPcm *playback, PcmQueue *queue, const volatile std::sig_atomic_t *running,
                    WavRecorder *recorder, AutoLevel *autoLevel) {
    short pcm[SerialDV::MBE_AUDIO_BLOCK_SIZE];
    // Start "idle": nothing's been heard yet, so there's nothing to backfill.
    auto lastRealFrame = std::chrono::steady_clock::now() - std::chrono::seconds(10);

    // Everything played also goes to the recorder (before the speaker
    // volume), which records it while the Record button is on.
    auto writeFrame = [&](const short *frame) {
        playback->write(frame);
        if (recorder) recorder->write(frame, SerialDV::MBE_AUDIO_BLOCK_SIZE);
    };

    while (*running) {
        if (queue->pop(pcm, 20)) {
            auto now = std::chrono::steady_clock::now();
            // The first frame after a pause: see restartIfStopped().
            if (now - lastRealFrame >= std::chrono::milliseconds(500)) {
                playback->restartIfStopped();
                if (autoLevel) autoLevel->reset(); // a new over, maybe a different station
            }
            lastRealFrame = now;
            if (autoLevel) autoLevel->process(pcm, SerialDV::MBE_AUDIO_BLOCK_SIZE);
            writeFrame(pcm);
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
        //
        // And only when the device is actually about to run dry: a frame
        // just a few ms late (common at the start of an over, while the
        // vocoder pipeline and the network settle) used to get 20 ms of
        // silence written ahead of it regardless -- while the device still
        // had 100+ ms queued, or hadn't even started playing (the start
        // threshold) -- chopping zeros in between the over's first frames:
        // the "blip" heard at the start of some overs, seen in recordings
        // as all-zero frames alternating with speech.
        if (std::chrono::steady_clock::now() - lastRealFrame < std::chrono::milliseconds(500) &&
            playback->runningLow(2 * SerialDV::MBE_AUDIO_BLOCK_SIZE)) {
            std::memset(pcm, 0, sizeof(pcm));
            writeFrame(pcm);
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
