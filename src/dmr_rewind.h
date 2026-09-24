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
// Wire format confirmed against BrandMeister's own reference Go
// implementation, github.com/BrandMeister/go-brandmeister (rewind/ and
// dmr/ packages), including its unit tests' literal packet bytes -- not
// guessed. Two things worth flagging for whoever picks this up next:
//
//  - Voice content: with the default Options (SuperHeader requested,
//    LinearFrame NOT requested -- see Option below), voice arrives as
//    DMRData sub-packets (VoiceHeader / VoiceFrameA-F / TerminatorLC)
//    carrying the SAME on-air DMR burst content dmr_voice.h already
//    knows how to build/parse for Homebrew (BPTC/Golay/Hamming FEC and
//    all) -- just wrapped in Rewind's envelope instead of a DMRD packet.
//    That's a genuine, unplanned code-reuse opportunity: going in, this
//    was expected to be a much simpler, FEC-free wire format
//    (OptionLinearFrame exists for exactly that, but isn't the default,
//    and its exact byte layout isn't confirmed by anything read so far).
//  - Not yet implemented here: actually decoding those DMRData sub-
//    packets into AMBE frames, and wiring this into DmrTransport. This
//    file is a first spike -- connect, authenticate, subscribe, and dump
//    incoming SuperHeaders/DMRData/DMRAudio -- meant to be run against
//    real BrandMeister (via odt_test.cpp) and cross-checked with a
//    packet capture before going further, the same way Homebrew's real
//    protocol quirks (RPTACK length matching, the Software/Package ID
//    allowlist) were found.
//
// The outer envelope (the 18-byte header, and Configuration/Subscription/
// SuperHeader payloads) is little-endian; the DMR burst content inside a
// DMRData payload is the usual big-endian on-air DMR bit layout, same as
// what dmr_voice.h builds for Homebrew.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <sys/types.h> // ssize_t
#include <vector>

namespace dmr::rewind {

// 54006, confirmed against BrandMeister's own "Network Ports" wiki page
// (via search snippets -- the page itself is behind an anti-bot gate I
// couldn't get past directly) as the Open DMR Terminal port, distinct
// from 54005 (Simple External Application/SAP -- a different, separately
// admin-provisioned protocol sharing the same underlying Rewind
// envelope). go-brandmeister's own DefaultPort constant is 54005, which
// makes sense given that library frames itself as an SEA client -- worth
// double-checking against BrandMeister's SelfCare panel or support if
// this doesn't connect.
constexpr uint16_t DEFAULT_PORT = 54006;
constexpr int KEEPALIVE_INTERVAL_SEC = 5; // matches go-brandmeister's DefaultKeepAliveInterval
constexpr int TIMEOUT_SEC = 15;           // matches go-brandmeister's DefaultTimeout
constexpr size_t CALL_LENGTH = 10;

enum class SessionType : uint32_t { PrivateVoice = 5, GroupVoice = 7 };

// VersionData's `service` byte -- confirmed against BrandMeister's own
// official reference client (github.com/BrandMeister/DigestPlay, ODMRTP
// branch, RewindClient.c's CreateRewindContext): REWIND_SERVICE_OPEN_TERMINAL
// = REWIND_ROLE_APPLICATION(0x20) + 1 = 0x21. Getting this wrong (0x20,
// go-brandmeister's generic "simple application" service code) is why an
// earlier version of this client got total silence back from a real
// master -- no Challenge, nothing -- across two different masters: the
// server appears to reject an unrecognised service type before auth is
// ever evaluated, rather than replying with any kind of error.
constexpr uint8_t SERVICE_OPEN_TERMINAL = 0x21;

// Client-declared feature options, sent in a Configuration packet.
enum Option : uint32_t {
    OptionSuperHeader = 1u << 0, // ask the server to send SuperHeader metadata
    OptionLinearFrame = 1u << 1, // ask for FEC-free AMBE instead of full on-air bursts -- NOT used here, see file header
};

// Packet type constants (ClassRewindControl=0x0000, ClassApplication=0x0900).
enum PacketType : uint16_t {
    TypeKeepAlive = 0x0000,
    TypeClose = 0x0001,
    TypeChallenge = 0x0002,
    TypeAuthentication = 0x0003,
    TypeConfiguration = 0x0900,
    TypeSubscription = 0x0901,
    TypeDMRDataBase = 0x0910,  // + (dataType & 0x0f) -- PI header/voice header/terminator LC/voice frames A-F/...
    TypeDMRAudioBase = 0x0920, // + subtype -- only used if OptionLinearFrame is requested
    TypeDMREmbeddedData = 0x0927,
    TypeSuperHeader = 0x0928,
    TypeFailureCode = 0x0929,
};

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

class RewindClient {
public:
    bool open(const std::string &host, uint16_t port = DEFAULT_PORT);

    // password is BrandMeister's "Hotspot Security" password from
    // SelfCare -- the same one an existing Homebrew/MMDVM connection
    // already uses, not the account password (confirmed against
    // VoxDMR's own setup docs, which use this same protocol).
    void setIdentity(uint32_t remoteId, const std::string &password, const std::string &description);
    void setOptions(uint32_t options) { m_options = options; }

    // Handshake: send an initial KeepAlive, wait for the server's
    // Challenge, respond with SHA256(challenge ++ password), then wait
    // for the server's Configuration ack (which is what marks a
    // connection authenticated, per go-brandmeister's own client).
    // Retries the initial KeepAlive a few times, matching
    // DmrClient::link()'s pattern, since a plain UDP send can silently
    // vanish.
    bool link();

    void subscribe(uint32_t targetId, SessionType type);

    void disconnect();

    void setSuperHeaderSink(std::function<void(const SuperHeaderInfo &)> sink);
    // dataType is the DMR PDU type nibble (VoiceHeader=1, TerminatorLC=2,
    // VoiceFrameA..F=0x0A..0x0F, etc. -- see go-brandmeister's dmr/type.go)
    // -- raw and unparsed for now, see this file's header comment.
    void setDmrDataSink(std::function<void(uint8_t dataType, const uint8_t *data, size_t len)> sink);
    // Only fires if OptionLinearFrame was requested (not the default here).
    void setDmrAudioSink(std::function<void(uint8_t subType, const uint8_t *data, size_t len)> sink);

    // Keepalive/receive loop; returns when dmr::g_running is cleared or
    // the server goes quiet for longer than TIMEOUT_SEC.
    void run();

private:
    ssize_t recvWithTimeout(uint8_t *buf, size_t len, int timeoutMs);
    void sendPacket(uint16_t type, const std::vector<uint8_t> &payload);
    void sendKeepAlive();
    void sendConfiguration();
    void handlePacket(const uint8_t *data, size_t len);

    int m_fd = -1;
    uint32_t m_remoteId = 0;
    std::string m_password;
    std::string m_description;
    uint32_t m_options = OptionSuperHeader;
    uint32_t m_sequence = 0;
    bool m_authenticated = false;

    std::function<void(const SuperHeaderInfo &)> m_superHeaderSink;
    std::function<void(uint8_t, const uint8_t *, size_t)> m_dmrDataSink;
    std::function<void(uint8_t, const uint8_t *, size_t)> m_dmrAudioSink;
};

} // namespace dmr::rewind
