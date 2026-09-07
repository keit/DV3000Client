#include "dextra_audio.h"

#include <chrono>
#include <cstdio>
#include <cstring>

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
    err = snd_pcm_set_params(m_handle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                              1, rate, 1, 4 * (1000000 / (rate / SerialDV::MBE_AUDIO_BLOCK_SIZE)));
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

    while (g_running) {
        if (!capture->read(pcm)) continue;

        bool active = pttActive();
        if (active && !transmitting) {
            streamId = client->beginLiveTx();
            transmitting = true;
        } else if (!active && transmitting) {
            client->endLiveTx(streamId);
            transmitting = false;
        }
        if (!transmitting) continue;

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

    if (transmitting) client->endLiveTx(streamId);
}

void playbackThread(AlsaPcm *playback, PcmQueue *queue) {
    short pcm[SerialDV::MBE_AUDIO_BLOCK_SIZE];
    while (g_running) {
        if (queue->pop(pcm, 100)) {
            playback->write(pcm);
        }
    }
}

} // namespace dextra
