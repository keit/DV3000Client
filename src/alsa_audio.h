#pragma once

// Protocol-independent ALSA audio for live mode: the capture/playback
// streams, the queue that carries decoded RX audio to the playback
// thread, the playback thread itself, and the capture loops' shared
// pacing throttle. D-Star (dextra_audio) and DMR (dmr_audio) differ only
// in how they turn captured PCM into network frames and back, which stays
// in those modules. Everything here is the same 8kHz/16-bit-mono, one
// period per 20ms DV frame, whatever vocoder rate the frames end up at.

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <mutex>
#include <string>

#include <alsa/asoundlib.h>

#include "dvcontroller.h"

namespace audio {

// Thin wrapper around one ALSA capture or playback stream, opened at
// 8kHz/16-bit-mono with a period matching one DV frame (160 samples =
// 20ms -- SerialDV::MBE_AUDIO_BLOCK_SIZE). read()/write() block for that
// period, which is what paces the capture/playback threads in live mode.
class AlsaPcm {
public:
    bool open(const std::string &device, snd_pcm_stream_t stream);
    // Discards anything still buffered -- call drain() first to play it out.
    void close();
    // Playback only: blocks until everything written has been played.
    void drain();

    // Reads/writes exactly SerialDV::MBE_AUDIO_BLOCK_SIZE samples,
    // recovering from over/underruns rather than treating them as fatal.
    bool read(short *pcm);
    bool write(const short *pcm);

    // Linear gain applied to everything read()/written from now on (see
    // audio_gain.h) -- how the GUI's mic and speaker volume sliders take
    // effect mid-transmission, without the capture/playback threads
    // needing to know about them. Atomic since the GUI thread sets it
    // while those threads are inside read()/write().
    void setGain(float gain) { m_gain.store(gain); }

    // Peak level (0..100, after gain) of everything read()/written since
    // the last call, then resets -- what the GUI's level meters poll.
    int takePeak() { return m_peak.exchange(0); }

private:
    bool recover(int err);

    void notePeak(const short *pcm);

    std::atomic<float> m_gain{1.0f};
    std::atomic<int> m_peak{0};

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

// Live-mode RX playback: drains decoded PCM chunks pushed by the network
// thread, backfilling brief gaps within a transmission with silence and
// leaving the device alone once things have gone quiet (see the .cpp for
// why both matter). Runs until *running is cleared -- each protocol
// passes its own flag (dextra::g_running / dmr::g_running) so the two
// sides' sessions can be stopped independently.
void playbackThread(AlsaPcm *playback, PcmQueue *queue, const volatile std::sig_atomic_t *running);

// The capture loops' pacing guard. AlsaPcm::read() blocking for one 20ms
// period is what normally paces a capture loop, but a stand-in device
// (e.g. ALSA's "null", used when no mic is attached yet; confirmed via
// `arecord -D null -d 3` returning in ~4ms instead of 3s) returns
// instantly instead, spinning the thread at 100% CPU with no pacing of
// its own.
//
// Padding every iteration up to a fixed period would fix that but break
// real hardware: a device catching up after a brief timing hiccup (e.g. a
// wireless dongle) legitimately returns faster than one period for a call
// or two while draining backlog, and forcing a sleep there would just
// prevent that catch-up, growing the backlog until the capture ring
// buffer overruns instead. So this only sleeps once several *consecutive*
// reads return near-instantly -- real hardware never sustains that under
// normal jitter, only a genuinely non-blocking device does.
//
// And only while *not* transmitting. Confirmed by a real capture xrun
// (missing words) against a network ThumbDV (AMBEServer 3000): a backlog
// builds in ALSA's own ring buffer often enough on its own that the
// throttle fired during real transmit too, adding its 20ms on top of
// encode()'s own ~15-20ms network round trip per frame. While
// transmitting, every ms counts against the capture buffer, so the loop
// is left to drain the backlog as fast as it can.
class CaptureThrottle {
public:
    // Call once per loop iteration, right after capture->read(), with how
    // long that read took and whether PTT is active this iteration.
    void afterRead(std::chrono::steady_clock::duration readElapsed, bool transmitting);

private:
    int m_fastReadStreak = 0;
};

} // namespace audio
