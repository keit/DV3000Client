#pragma once

// Keeps several ThumbDV requests in flight instead of waiting for each
// reply before sending the next. One frame per round trip only keeps up
// while a round trip takes under 20 ms (one audio frame); a ThumbDV behind
// an AMBEServer runs 15-18.5 ms even when well set up, and 32-35 ms when
// the Pi's FTDI latency timer is at its 16 ms default -- so audio fell
// further behind with every frame and broke up, while BlueDV, which
// doesn't wait frame by frame, stayed clear on the same setup. With
// requests pipelined, a slow link only delays audio by its round trip.
//
// Protocol-independent: D-Star and DMR, transmit and receive all submit
// here, from whichever threads they run on; results come back, in the
// order submitted, on this class's own reply thread.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include "dvcontroller.h"

namespace audio {

class VocoderPipeline {
public:
    using EncodeDone = std::function<void(const uint8_t *ambe)>; // the rate's AMBE frame (9 bytes for both D-Star and DMR)
    using DecodeDone = std::function<void(const short *pcm)>;    // SerialDV::MBE_AUDIO_BLOCK_SIZE samples

    // Takes over dv (open) for the session at rate: until this is
    // destroyed, nothing else may call dv's encode()/decode(). ok() says
    // whether the vocoder accepted the rate.
    VocoderPipeline(SerialDV::DVController &dv, SerialDV::DVRate rate);
    // Waits for what's still in flight (see drain()), then hands dv back.
    ~VocoderPipeline();
    VocoderPipeline(const VocoderPipeline &) = delete;
    VocoderPipeline &operator=(const VocoderPipeline &) = delete;

    bool ok() const { return m_ok; }

    // Send a request now; done runs on the reply thread when its reply
    // arrives, after every earlier request's -- not at all if the request
    // fails, so the frame is simply dropped, as before. done must not
    // submit or drain (it runs on the thread they'd wait for). Returns
    // false, dropping the frame, if the pipeline stayed full for 100 ms.
    bool encode(const short *pcm, EncodeDone done);
    bool decode(const uint8_t *ambe, DecodeDone done);

    // Waits (at most 1 s) until every request submitted so far has been
    // answered or given up on -- e.g. before a transmission's terminator,
    // which must follow its last voice frame.
    void drain();

private:
    struct Job {
        bool encode;
        std::chrono::steady_clock::time_point sent;
        EncodeDone encodeDone;
        DecodeDone decodeDone;
    };

    bool submit(Job job, const void *data);
    void replyLoop();

    // Generous: in steady state about one round trip's worth of frames is
    // in flight (2-3 at 30-60 ms; DMR submits a burst's 3 at once).
    static constexpr size_t kMaxInFlight = 8;

    SerialDV::DVController &m_dv;
    bool m_ok;
    std::mutex m_sendMutex; // keeps the wire order and m_inFlight's order the same
    std::mutex m_mutex;     // guards everything below
    std::condition_variable m_changed;
    std::deque<Job> m_inFlight;
    // After a failed reply: no new requests until everything in flight has
    // been collected and stale input discarded, since replies carry nothing
    // to match them to their requests (see DVController::beginPipelining).
    bool m_recovering = false;
    // When m_inFlight last became empty. A missing reply can't be detected
    // while later ones keep arriving (the next one is simply taken in its
    // place, harmless within a stream, where every frame goes to the same
    // place in order) -- but a stray reply still waiting when the next
    // transmission starts would put old audio at its start and shift it by
    // a frame. So input is cleared before sending after a pause.
    std::chrono::steady_clock::time_point m_idleSince = std::chrono::steady_clock::now();
    bool m_stopping = false;
    std::chrono::steady_clock::time_point m_lastDropLog;
    std::thread m_thread;
};

} // namespace audio
