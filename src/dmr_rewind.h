#pragma once

// BrandMeister's "Open DMR Terminal" protocol -- their own lighter,
// application-level interface for IP-based terminals (hotspot-style
// clients), built on their self-written "Rewind" protocol over UDP. A
// wire format entirely independent of Homebrew/MMDVM (dmr_client.h):
// different envelope, different challenge/response auth, and -- per
// BrandMeister support (2026-09), who declined to hand out the
// Homebrew-side Software/Package ID pair DmrClient currently uses --
// their recommended path for a new client going forward.
//
// Confirmed working end to end against real BrandMeister (RX: heard a
// live transmission from a Pi-Star hotspot; TX: see below), cross-checked
// against three independent real clients: BrandMeister's own official
// reference (github.com/BrandMeister/DigestPlay, ODMRTP branch) and two
// community ones (github.com/BrandMeister/go-brandmeister,
// github.com/abo4/pyspot_rx). Two real protocol details only found
// through live testing, not documentation:
//
//  - VersionData's `service` byte must be SERVICE_OPEN_TERMINAL (0x21),
//    not the generic "simple application" code (0x20) -- getting this
//    wrong gets total silence back, not even a Challenge.
//  - The first field of the initial KeepAlive's VersionData is the
//    terminal's DMR ID (per the Open DMR Terminal protocol document; the
//    reference clients' own names for it -- "RemoteID"/"number" in
//    DigestPlay and go-brandmeister, "terminal_id" in pyspot_rx -- are
//    misleading). It must be the caller's real DMR ID, not an arbitrary
//    application ID: an unrelated value passes the full challenge/
//    response handshake but then gets silently dropped rather than
//    subscribed.
//
// Voice content: with the default Options (SuperHeader requested,
// LinearFrame not), voice is NOT the FEC-wrapped on-air burst format
// dmr_voice.h builds for Homebrew, despite DMRData's VoiceHeader/
// TerminatorLC sub-types looking like they'd carry that -- it's plain
// AMBE instead, confirmed live: each DMRAudio packet is exactly 3
// concatenated 9-byte AMBE+2 half-rate frames (27 bytes), matching
// dmr::AMBE_FRAME_SIZE and DigestPlay's own MODE33_FRAME_SIZE*3. That's
// simpler than Homebrew's framing, and is why setVoiceRxSink()/
// sendVoiceFrame() here work directly with dmr_audio.h's existing
// protocol-agnostic capture/playback threads.
//
// The outer envelope (the 18-byte header, and Configuration/Subscription/
// SuperHeader payloads) is little-endian; DMRData's VoiceHeader/
// TerminatorLC payloads use the usual big-endian on-air DMR field layout
// (FLCO/FID/ServiceOptions/DestinationID/SourceID, plus 3 trailing bytes
// that both this implementation and DigestPlay's reference leave as
// zero on transmit and don't validate on receive).
//
// Two sequence counters, not one: DigestPlay's reference keeps a separate
// counter for "real-time" packets (voice header/audio/terminator) from
// the "routine" one (KeepAlive/Authentication/Subscription/Configuration),
// selected by the RealTime1 flag bit -- replicated here as m_sequence /
// m_sequenceRealTime.

#include "dmr_transport.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <sys/types.h> // ssize_t
#include <vector>

namespace dmr::rewind {

// 54006, confirmed against BrandMeister's own "Network Ports" wiki page
// (via search snippets -- the page itself is behind an anti-bot gate) as
// the Open DMR Terminal port, distinct from 54005 (Simple External
// Application/SAP -- a different, separately admin-provisioned protocol
// sharing the same underlying Rewind envelope).
constexpr uint16_t DEFAULT_PORT = 54006;
constexpr int KEEPALIVE_INTERVAL_SEC = 5; // matches go-brandmeister's DefaultKeepAliveInterval
constexpr int TIMEOUT_SEC = 15;           // matches go-brandmeister's DefaultTimeout
constexpr size_t CALL_LENGTH = 10;

enum class SessionType : uint32_t { PrivateVoice = 5, GroupVoice = 7 };

// VersionData's `service` byte -- see this file's header comment.
constexpr uint8_t SERVICE_OPEN_TERMINAL = 0x21;

// Client-declared feature options, sent in a Configuration packet.
enum Option : uint32_t {
    OptionSuperHeader = 1u << 0, // ask the server to send SuperHeader metadata
    OptionLinearFrame = 1u << 1, // an alternative voice framing, not used here -- see file header
};

// Packet type constants (ClassRewindControl=0x0000, ClassApplication=0x0900).
enum PacketType : uint16_t {
    TypeKeepAlive = 0x0000,
    TypeClose = 0x0001,
    TypeChallenge = 0x0002,
    TypeAuthentication = 0x0003,
    TypeConfiguration = 0x0900,
    TypeSubscription = 0x0901,
    TypeCancelling = 0x0902, // unsubscribe -- see unsubscribe() below
    TypeDMRDataBase = 0x0910,  // + dataType -- 1=VoiceHeader, 2=TerminatorLC, ...
    TypeDMRAudioBase = 0x0920, // + subtype -- the actual voice payload, see file header
    TypeDMREmbeddedData = 0x0927,
    TypeSuperHeader = 0x0928,
    TypeFailureCode = 0x0929,
};

// DMRData sub-types relevant to voice (see go-brandmeister's dmr/type.go
// for the full list, including non-voice ones this client ignores).
constexpr uint8_t DMR_DATA_VOICE_HEADER = 1;
constexpr uint8_t DMR_DATA_TERMINATOR_LC = 2;

constexpr uint16_t FlagRealTime1 = 1u << 0;

// Metadata BrandMeister sends once per transmission (if OptionSuperHeader
// was requested) -- notably including plaintext callsigns, something
// Homebrew's wire format never carries.
struct SuperHeaderInfo {
    uint32_t sessionType = 0;
    uint32_t source = 0;
    uint32_t target = 0;
    std::string sourceCall;
    std::string targetCall;
};

// Implements dmr::DmrTransport so callers (dmr_audio.h's
// captureThread/playbackThread, and eventually the GUI) can use this
// interchangeably with the Homebrew DmrClient. Connection setup (host,
// identity, options) stays outside the interface, same reasoning as
// DmrClient -- see dmr_transport.h.
class RewindClient : public DmrTransport {
public:
    bool open(const std::string &host, uint16_t port = DEFAULT_PORT);

    // dmrId is the terminal's DMR ID, sent in the KeepAlive and also used
    // as the source ID of anything transmitted -- see file header.
    // password is BrandMeister's "Hotspot Security" password from
    // SelfCare -- the same one an existing Homebrew/MMDVM connection
    // already uses, not the account password.
    void setIdentity(uint32_t dmrId, const std::string &password, const std::string &description);
    void setOptions(uint32_t options) { m_options = options; }

    // Handshake: send an initial KeepAlive, wait for the server's
    // Challenge, respond with SHA256(challenge ++ password), then wait
    // for either a Configuration or a plain KeepAlive back (real clients
    // differ on which marks success -- see dmr_rewind.cpp's comment).
    // Resends KeepAlive every iteration while waiting, matching
    // DigestPlay's reference client.
    LinkResult link() override;

    // Subscriptions are additive, confirmed live -- subscribing to a new
    // target does NOT drop a previous one; callers that want to actually
    // switch talkgroups need to unsubscribe() the old target themselves.
    void subscribe(uint32_t targetId, SessionType type);
    void unsubscribe(uint32_t targetId, SessionType type);

    void disconnect() override;

    void setSuperHeaderSink(std::function<void(const SuperHeaderInfo &)> sink);
    // dataType is the DMR PDU type nibble (see DMR_DATA_* above and
    // go-brandmeister's dmr/type.go for the full list) -- raw and
    // unparsed; mainly useful for diagnostics since the fields that
    // matter (addressing, voice) are already surfaced via
    // setHeaderSink()/setSuperHeaderSink()/setVoiceRxSink().
    void setDmrDataSink(std::function<void(uint8_t dataType, const uint8_t *data, size_t len)> sink);
    // Fires once per incoming transmission. Sourced from the DMRData
    // VoiceHeader (which carries the source and destination IDs), NOT
    // from SuperHeader -- confirmed live that this server never sends a
    // SuperHeader even though it's requested, so relying on it left
    // Last Heard permanently empty. VoiceHeader is resent 3x per call,
    // so notifyHeader() dedupes; a SuperHeader, if one ever does arrive,
    // goes through the same dedupe.
    void setHeaderSink(std::function<void(uint32_t srcId, uint32_t dstId)> sink) override;
    // Called with each transmission's 3 AMBE half-rate frames, split out
    // of a 27-byte DMRAudio packet -- see file header.
    void setVoiceRxSink(std::function<void(const uint8_t *, const uint8_t *, const uint8_t *)> sink) override;

    // Live-mode TX. streamId is accepted only for DmrTransport
    // conformance -- Rewind has no stream-ID concept on the wire (a
    // transmission is just "whatever VoiceHeader was most recently sent
    // on this authenticated session"), so it's otherwise unused.
    uint32_t beginVoiceTx(uint32_t dstId, dmr::CallType callType) override;
    void sendVoiceFrame(uint32_t streamId, const uint8_t ambe0[dmr::AMBE_FRAME_SIZE],
                         const uint8_t ambe1[dmr::AMBE_FRAME_SIZE], const uint8_t ambe2[dmr::AMBE_FRAME_SIZE]) override;
    void endVoiceTx(uint32_t streamId) override;

    // Keepalive/receive loop; returns when dmr::g_running is cleared or
    // the server goes quiet for longer than TIMEOUT_SEC.
    void run() override;

private:
    ssize_t recvWithTimeout(uint8_t *buf, size_t len, int timeoutMs);
    void sendPacket(uint16_t type, const std::vector<uint8_t> &payload, uint16_t flags = 0);
    void sendKeepAlive();
    void sendConfiguration();
    void handlePacket(const uint8_t *data, size_t len);
    void notifyHeader(uint32_t srcId, uint32_t dstId);

    int m_fd = -1;
    uint32_t m_dmrId = 0;
    std::string m_password;
    std::string m_description;
    uint32_t m_options = OptionSuperHeader;
    // Atomic: unlike beginVoiceTx/sendVoiceFrame/endVoiceTx (only ever
    // called from one capture thread), subscribe() is meant to be safe
    // to call from whatever thread owns the UI (e.g. to resubscribe when
    // the user changes talkgroup) concurrently with run()'s own thread
    // sending periodic KeepAlives -- both share m_sequence.
    std::atomic<uint32_t> m_sequence{0};         // routine packets (KeepAlive, Authentication, Subscription, Configuration)
    std::atomic<uint32_t> m_sequenceRealTime{0}; // voice header/audio/terminator -- see file header
    bool m_authenticated = false;

    uint32_t m_txDstId = 0;
    bool m_txPrivateCall = false;
    uint32_t m_txStreamCounter = 0;

    // notifyHeader()'s dedupe state -- network thread only.
    bool m_haveHeader = false;
    uint32_t m_lastHeaderSrc = 0, m_lastHeaderDst = 0;
    std::chrono::steady_clock::time_point m_lastActivity;

    // Receive logging: one line when an incoming call starts and one when
    // it ends, saying how -- terminator, cut off by the next call, or
    // audio just stopping. Without these, "BrandMeister stopped sending us
    // a talkgroup" and "audio arrived but went unheard" look the same in
    // the log (it happened: two minutes of a QSO missing, heard fine on a
    // hotspot on the same talkgroup).
    void endRxCall(const char *how);
    bool m_rxInCall = false;
    uint32_t m_rxSrc = 0, m_rxDst = 0;
    int m_rxPackets = 0; // DMRAudio packets, 3 AMBE frames (60 ms) each
    std::chrono::steady_clock::time_point m_rxStart;

    std::function<void(const SuperHeaderInfo &)> m_superHeaderSink;
    std::function<void(uint8_t, const uint8_t *, size_t)> m_dmrDataSink;
    std::function<void(uint32_t, uint32_t)> m_headerSink;
    std::function<void(const uint8_t *, const uint8_t *, const uint8_t *)> m_voiceRxSink;
};

} // namespace dmr::rewind
