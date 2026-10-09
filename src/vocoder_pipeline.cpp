#include "vocoder_pipeline.h"

#include <cstdio>

namespace audio {

VocoderPipeline::VocoderPipeline(SerialDV::DVController &dv, SerialDV::DVRate rate)
    : m_dv(dv), m_ok(dv.beginPipelining(rate)) {
    m_thread = std::thread(&VocoderPipeline::replyLoop, this);
}

VocoderPipeline::~VocoderPipeline() {
    drain();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
    }
    m_changed.notify_all();
    m_thread.join();
    m_dv.endPipelining();
}

bool VocoderPipeline::encode(const short *pcm, EncodeDone done) {
    return submit(Job{true, {}, std::move(done), nullptr}, pcm);
}

bool VocoderPipeline::decode(const uint8_t *ambe, DecodeDone done) {
    return submit(Job{false, {}, nullptr, std::move(done)}, ambe);
}

bool VocoderPipeline::submit(Job job, const void *data) {
    const bool isEncode = job.encode;
    std::lock_guard<std::mutex> sendLock(m_sendMutex);
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        bool room = m_changed.wait_for(lock, std::chrono::milliseconds(100), [this] {
            return m_stopping || (!m_recovering && m_inFlight.size() < kMaxInFlight);
        });
        if (!room || m_stopping) {
            auto now = std::chrono::steady_clock::now();
            if (now - m_lastDropLog > std::chrono::seconds(5)) {
                std::fprintf(stderr, "vocoder: ThumbDV not keeping up, dropping %s frames\n",
                             isEncode ? "transmit" : "receive");
                m_lastDropLog = now;
            }
            return false;
        }
        job.sent = std::chrono::steady_clock::now();
        if (m_inFlight.empty() && job.sent - m_idleSince > std::chrono::milliseconds(100))
            m_dv.discardPendingReplies(); // nothing in flight, so anything waiting is a stray
        m_inFlight.push_back(std::move(job));
    }
    // Sent after queueing, so the reply thread is already expecting it;
    // under m_sendMutex, so two submitting threads can't send in a
    // different order from the one they queued in.
    if (isEncode)
        m_dv.sendEncode(static_cast<const short *>(data));
    else
        m_dv.sendDecode(static_cast<const unsigned char *>(data));
    m_changed.notify_all();
    return true;
}

void VocoderPipeline::drain() {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_changed.wait_for(lock, std::chrono::seconds(1), [this] { return m_inFlight.empty() && !m_recovering; });
}

void VocoderPipeline::replyLoop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_changed.wait(lock, [this] { return m_stopping || !m_inFlight.empty(); });
            if (m_inFlight.empty()) return; // stopping, nothing left
            // Copied, not popped: it still counts as in flight until its
            // reply is in and done() has run.
            job = m_inFlight.front();
        }

        bool ok;
        if (job.encode) {
            uint8_t ambe[SerialDV::MBE_FRAME_MAX_LENGTH_BYTES];
            ok = m_dv.receiveEncode(ambe);
            if (ok) {
                m_dv.noteRoundTrip(true, job.sent);
                if (job.encodeDone) job.encodeDone(ambe);
            }
        } else {
            short pcm[SerialDV::MBE_AUDIO_BLOCK_SIZE];
            ok = m_dv.receiveDecode(pcm);
            if (ok) {
                m_dv.noteRoundTrip(false, job.sent);
                if (job.decodeDone) job.decodeDone(pcm);
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_inFlight.pop_front();
            if (m_inFlight.empty()) m_idleSince = std::chrono::steady_clock::now();
            if (!ok) m_recovering = true;
            // Everything that was in flight is collected, so anything still
            // arriving can only be a straggler: drop it before sending again.
            if (m_recovering && m_inFlight.empty()) {
                m_dv.discardPendingReplies();
                m_recovering = false;
            }
        }
        m_changed.notify_all();
    }
}

} // namespace audio
