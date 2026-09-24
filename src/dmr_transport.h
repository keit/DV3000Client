#pragma once

// Abstract interface for a live DMR voice session, covering exactly what
// dmr_audio.cpp's capture/playback threads and the GUI need once a session
// is up -- independent of which wire protocol is actually carrying it.
// DmrClient (dmr_client.h) implements this for Homebrew/MMDVM; a planned
// Open DMR Terminal backend (BrandMeister's own lighter protocol, built on
// their Rewind protocol over UDP -- see project notes) will implement it
// too. The two share almost nothing at the framing level (Homebrew's DMRD
// carries a full on-air burst with BPTC/Golay/Hamming FEC, since it's
// designed to make a hotspot look like a real repeater relaying RF; Open
// DMR Terminal is an application-level protocol with no such pretense) --
// this interface is deliberately just the handful of operations callers
// actually need, not an attempt to unify the wire formats.
//
// Connection setup (host/port, credentials, repeater config vs. whatever
// Open DMR Terminal turns out to need) is deliberately NOT part of this
// interface -- those are too backend-specific to unify usefully, so
// callers construct and configure a concrete backend, then use it through
// this interface once connected.

#include "dmr_voice.h" // CallType, AMBE_FRAME_SIZE

#include <csignal>
#include <cstdint>
#include <functional>

namespace dmr {

// Cleared by SIGINT/SIGTERM (installed by the frontend) to unwind a
// backend's run() loop cooperatively -- same pattern as dextra::g_running,
// but independent of it: DMR code has no dependency on dextra_client.h.
// Shared by every backend (Homebrew's DmrClient, and any future one),
// which is why it lives here rather than in dmr_client.h.
extern volatile sig_atomic_t g_running;

// Shared by every backend's link(), so it lives here rather than in
// dmr_client.h -- the RPT-flavored member names are Homebrew's own
// handshake steps, but Success/Timeout (and, loosely, AuthRejected) are
// generic enough for a non-Homebrew backend to reuse rather than
// invent a parallel enum. See dmr_client.cpp's ToString() for the
// Homebrew-specific wording of each.
enum class LinkResult {
    Success,
    LoginRejected,  // DMR ID not recognised/permitted
    AuthRejected,   // wrong password
    ConfigRejected, // master rejected the declared config
    Timeout,
};

const char *ToString(LinkResult result);

class DmrTransport {
public:
    virtual ~DmrTransport() = default;

    // Runs the backend's handshake/login sequence.
    virtual LinkResult link() = 0;

    virtual void disconnect() = 0;

    // Live-mode RX: called with each incoming transmission's 3 AMBE
    // half-rate frames as they arrive, for immediate decode/playback.
    virtual void setVoiceRxSink(std::function<void(const uint8_t *, const uint8_t *, const uint8_t *)> sink) = 0;

    // Fires once per incoming transmission, as early as the backend can
    // determine (srcId, dstId) -- for a Last Heard log.
    virtual void setHeaderSink(std::function<void(uint32_t srcId, uint32_t dstId)> sink) = 0;

    // Live-mode TX, driven by a capture thread's PTT state machine. dstId
    // is the talkgroup (Group call) or target DMR ID (Private call) to
    // transmit to, chosen per-transmission. Returns a fresh stream ID.
    virtual uint32_t beginVoiceTx(uint32_t dstId, dmr::CallType callType) = 0;

    // Sends one burst's 3 AMBE half-rate frames as part of stream
    // streamId. Any per-burst wire-framing state a backend needs (e.g.
    // Homebrew's 6-position sync/EMB cycle) is tracked internally --
    // callers don't need to know it exists.
    virtual void sendVoiceFrame(uint32_t streamId, const uint8_t ambe0[dmr::AMBE_FRAME_SIZE],
                                 const uint8_t ambe1[dmr::AMBE_FRAME_SIZE],
                                 const uint8_t ambe2[dmr::AMBE_FRAME_SIZE]) = 0;

    virtual void endVoiceTx(uint32_t streamId) = 0;

    // Keepalive/receive loop; returns when g_running is cleared.
    virtual void run() = 0;
};

} // namespace dmr
