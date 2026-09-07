#pragma once

// ALSA audio I/O for live mode: capture/playback streams and the threads
// that pump PCM through the ThumbDV and a DextraClient session. Kept
// separate from dextra_client.h/.cpp -- the DExtra protocol client has no
// business depending on ALSA specifically, and a frontend that wants a
// different audio backend (or none at all, e.g. file-based test modes)
// shouldn't have to link this.

#include <array>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>

#include <alsa/asoundlib.h>

#include "dextra_client.h"
#include "dvcontroller.h"

namespace dextra {

// Thin wrapper around one ALSA capture or playback stream, opened at
// D-Star's 8kHz/16-bit-mono rate with a period matching one DV frame (160
// samples = 20ms -- SerialDV::MBE_AUDIO_BLOCK_SIZE). read()/write() block
// for that period, which is what paces the capture/playback threads in
// live mode.
class AlsaPcm {
public:
    bool open(const std::string &device, snd_pcm_stream_t stream);
    void close();

    // Reads/writes exactly SerialDV::MBE_AUDIO_BLOCK_SIZE samples,
    // recovering from over/underruns rather than treating them as fatal.
    bool read(short *pcm);
    bool write(const short *pcm);

private:
    bool recover(int err);

    snd_pcm_t *m_handle = nullptr;
    snd_pcm_stream_t m_stream = SND_PCM_STREAM_CAPTURE;
};

// Bounded queue of decoded PCM chunks handed from the network thread
// (live-mode RX decode) to the playback thread.
class PcmQueue {
public:
    void push(const short *pcm);

    // Waits up to timeoutMs for a chunk; returns false on timeout.
    bool pop(short *pcm, int timeoutMs);

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::array<short, SerialDV::MBE_AUDIO_BLOCK_SIZE>> m_queue;
};

// Live-mode TX: PTT-driven mic -> ThumbDV -> network. Runs on its own
// thread; AlsaPcm::read() blocking for one ALSA period (20ms) is what
// paces it. pttActive is polled once per period to decide whether to
// originate/continue/end a transmission -- callers supply it (a terminal
// key state, a GUI button, ...) rather than this assuming any particular
// input device.
void captureThread(SerialDV::DVController *dv, AlsaPcm *capture, DextraClient *client,
                    std::function<bool()> pttActive);

// Live-mode RX playback: drains decoded PCM chunks pushed by the network
// thread's onFramePacket(). If nothing arrives for a while (no active RX
// stream), it just idles -- ALSA's own buffering absorbs the gap.
void playbackThread(AlsaPcm *playback, PcmQueue *queue);

} // namespace dextra
