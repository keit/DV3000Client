#pragma once

// DMR Homebrew/MMDVM repeater protocol client -- the protocol BrandMeister,
// DMR+, and other Homebrew-based DMR networks use for hotspot/repeater
// connections. A separate, independent protocol from DExtra/D-Star (see
// dextra_client.h) -- different transport framing, ID-based rather than
// callsign-based addressing, and a real login/auth handshake -- so this is
// a standalone client, not an extension of DextraClient.
//
// Covers the handshake and keepalive loop (RPTL -> RPTK -> RPTC, then
// RPTPING/MSTPONG) plus DMRD voice TX/RX (see dmr_voice.h for the actual
// burst framing -- BPTC/Golay/QR/Hamming FEC-encoded, unlike D-Star's much
// simpler raw-AMBE-in-a-packet framing). Not yet wired into a frontend
// (GUI integration, and driving voice TX from a real ThumbDV/PTT the way
// dextra_audio's captureThread does, are the remaining steps).
//
// Wire format confirmed against two independent, cross-checked sources:
// the vendored third_party/xlxd's server-side implementation
// (cdmrmmdvmprotocol.cpp) and g4klx/DMRGateway's client-side implementation
// (the software real BrandMeister hotspots run today).

#include "dmr_voice.h"

#include <cstdint>
#include <csignal>
#include <functional>
#include <string>

namespace dmr {

// Cleared by SIGINT/SIGTERM (installed by the frontend) to unwind run()'s
// loop cooperatively -- same pattern as dextra::g_running, but independent
// of it: this client has no dependency on dextra_client.h.
extern volatile sig_atomic_t g_running;

constexpr uint16_t DEFAULT_PORT = 62030;
constexpr int KEEPALIVE_PERIOD_SEC = 10; // matches DMRMMDVM_KEEPALIVE_PERIOD

enum class LinkResult {
    Success,
    LoginRejected,  // MSTNAK after RPTL -- DMR ID not recognised/permitted
    AuthRejected,   // MSTNAK after RPTK -- wrong password
    ConfigRejected, // MSTNAK after RPTC -- master rejected the declared config
    Timeout,
};

const char *ToString(LinkResult result);

// The RPTC packet's declared repeater info -- mostly cosmetic for a
// hotspot-style connection (shown on the network's dashboard). Mostly
// cosmetic, but not unvalidated: per the Homebrew protocol spec, the
// free-text fields (Location, Description, URL) must contain no HTML, no
// special/non-ASCII characters, and -- for URL specifically -- no
// advertisements or links unrelated to amateur radio. A real master (unlike
// xlxd's own minimal RPTC handling, which barely checks anything) can and
// does reject RPTC outright over content in these fields, which is why
// `url` defaults to empty rather than some placeholder link.
struct RepeaterConfig {
    std::string callsign;
    uint32_t rxFrequencyHz = 438800000;
    uint32_t txFrequencyHz = 438800000;
    unsigned power = 1;     // watts, 0-99
    unsigned colorCode = 1; // 0-15
    // Slot 2 is the confirmed convention for hotspot-style BrandMeister
    // connections (verified against a real, working Pi-Star session's own
    // config -- see dmr_voice.h's TimeSlot comment); Slot 1 exists for
    // masters/setups that expect it instead.
    dmr::TimeSlot timeSlot = dmr::TimeSlot::Slot2;
    float latitude = 0.0f;
    float longitude = 0.0f;
    int heightMeters = 0;
    std::string location = "Unknown";
    std::string description = "DV3000Client";
    std::string url; // optional per spec; left blank rather than a generic (non-ham-related) placeholder
};

class DmrClient {
public:
    bool open(const std::string &host, uint16_t port = DEFAULT_PORT);

    // dmrId is this client's own (repeater/hotspot) DMR ID. Must be called
    // before link().
    void setIdentity(uint32_t dmrId, const std::string &password, const RepeaterConfig &config);

    // Runs the full RPTL -> RPTK -> RPTC handshake, retrying each step
    // (matching DExtra's link() retry pattern) until it succeeds, is
    // rejected, or times out.
    LinkResult link();

    // Sends RPTCL and closes the socket.
    void disconnect();

    // Live-mode RX: called with each voice burst's 3 AMBE half-rate
    // frames as they arrive, for immediate decode/playback -- same shape
    // as DextraClient's liveRxSink. Optional; unset means received voice
    // is silently ignored (still logged via run()'s own diagnostics).
    void setVoiceRxSink(std::function<void(const uint8_t *, const uint8_t *, const uint8_t *)> sink);

    // Fires once per incoming transmission, when its Voice LC Header frame
    // arrives (before any voice bursts) -- gives (srcId, dstId) straight
    // from the DMRD packet fields, for a "last heard" log. Unlike D-Star's
    // header, this carries no callsign -- the Homebrew wire protocol is
    // purely numeric ID-based; a receiving client resolves callsigns (if
    // it wants to show one) via a separate public directory, same as
    // MMDVMHost/Pi-Star and xlxd do -- see src/gui/dmriddirectory.h.
    void setHeaderSink(std::function<void(uint32_t srcId, uint32_t dstId)> sink);

    // Live-mode TX, driven by a capture thread's PTT state machine --
    // same shape as DextraClient::beginLiveTx/sendLiveTxFrame/endLiveTx.
    // dstId is the talkgroup (Group call) or target DMR ID (Private call)
    // to transmit to, chosen per-transmission (dynamic TG selection, not a
    // static RPTO assignment -- see the project's protocol-roadmap notes
    // on why). Returns a fresh stream ID and sends the DMRD header frame.
    // callType defaults to Group since that's the overwhelmingly common
    // case; some network features (e.g. BrandMeister's Parrot echo test,
    // ID 9990) only respond to a genuine Private call.
    uint32_t beginVoiceTx(uint32_t dstId, dmr::CallType callType = dmr::CallType::Group);
    // frameInBurst cycles 0-5 across successive calls within one
    // transmission (see dmr_voice.h's buildVoiceFrame for what each
    // position means); callers don't need to track dstId or embeddedLC
    // themselves, beginVoiceTx() already captured both for the duration
    // of this transmission.
    void sendVoiceFrame(uint32_t streamId, int frameInBurst, const uint8_t ambe0[dmr::AMBE_FRAME_SIZE],
                         const uint8_t ambe1[dmr::AMBE_FRAME_SIZE], const uint8_t ambe2[dmr::AMBE_FRAME_SIZE]);
    void endVoiceTx(uint32_t streamId);

    // Keepalive/receive loop: sends RPTPING every KEEPALIVE_PERIOD_SEC,
    // delivers incoming DMRD voice frames to the RX sink (if set), and
    // logs anything else that comes back. Returns when g_running is cleared.
    void run();

private:
    ssize_t recvWithTimeout(uint8_t *buf, size_t len, int timeoutMs);
    void sendPing();
    void sendRaw(const std::vector<uint8_t> &packet);

    int m_fd = -1;
    uint32_t m_dmrId = 0;
    std::string m_password;
    RepeaterConfig m_config;
    uint8_t m_salt[4] = {};

    uint32_t m_txStreamCounter = 0;
    uint8_t m_txSeqId = 0;
    uint32_t m_txDstId = 0;
    dmr::TxParams m_txParams; // colorCode/timeSlot come from m_config, callType is set per-transmission
    dmr::EmbeddedLC m_txEmbeddedLC{};

    std::function<void(const uint8_t *, const uint8_t *, const uint8_t *)> m_voiceRxSink;
    std::function<void(uint32_t, uint32_t)> m_headerSink;
};

} // namespace dmr
