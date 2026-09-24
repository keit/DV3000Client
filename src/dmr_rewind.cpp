#include "dmr_rewind.h"

#include "dmr_transport.h" // dmr::g_running
#include "sha256.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace dmr::rewind {

namespace {

constexpr char SIGN[8] = {'R', 'E', 'W', 'I', 'N', 'D', '0', '1'};
constexpr size_t HEADER_LENGTH = 18; // Sign(8) + Type(2) + Flags(2) + Sequence(4) + Length(2), all little-endian

void appendU16LE(std::vector<uint8_t> &buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v));
    buf.push_back(static_cast<uint8_t>(v >> 8));
}

void appendU32LE(std::vector<uint8_t> &buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v));
    buf.push_back(static_cast<uint8_t>(v >> 8));
    buf.push_back(static_cast<uint8_t>(v >> 16));
    buf.push_back(static_cast<uint8_t>(v >> 24));
}

uint16_t readU16LE(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t readU32LE(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// Callsign fields are fixed-width, null-padded -- trims at the first zero
// byte, matching go-brandmeister's Call.String().
std::string callsignFromBytes(const uint8_t *bytes, size_t len) {
    size_t n = 0;
    while (n < len && bytes[n] != 0) n++;
    return std::string(reinterpret_cast<const char *>(bytes), n);
}

} // namespace

bool RewindClient::open(const std::string &host, uint16_t port) {
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *res = nullptr;
    char portStr[8];
    std::snprintf(portStr, sizeof(portStr), "%u", port);
    if (getaddrinfo(host.c_str(), portStr, &hints, &res) != 0 || !res) {
        std::fprintf(stderr, "dmr_rewind: cannot resolve %s\n", host.c_str());
        return false;
    }

    m_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (m_fd < 0) {
        std::fprintf(stderr, "dmr_rewind: socket() failed\n");
        freeaddrinfo(res);
        return false;
    }

    if (::connect(m_fd, res->ai_addr, res->ai_addrlen) < 0) {
        std::fprintf(stderr, "dmr_rewind: connect() failed\n");
        freeaddrinfo(res);
        return false;
    }
    freeaddrinfo(res);

    std::fprintf(stderr, "dmr_rewind: opened UDP socket to %s:%u\n", host.c_str(), port);
    return true;
}

void RewindClient::setIdentity(uint32_t remoteId, const std::string &password, const std::string &description) {
    m_remoteId = remoteId;
    m_password = password;
    m_description = description;
}

ssize_t RewindClient::recvWithTimeout(uint8_t *buf, size_t len, int timeoutMs) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(m_fd, &fds);
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int r = ::select(m_fd + 1, &fds, nullptr, nullptr, &tv);
    if (r <= 0) return 0;
    ssize_t n = ::recv(m_fd, buf, len, 0);
    if (n < 0) std::fprintf(stderr, "dmr_rewind: recv() error: %s\n", std::strerror(errno));
    return n;
}

void RewindClient::sendPacket(uint16_t type, const std::vector<uint8_t> &payload, uint16_t flags) {
    // Two independent sequence counters, selected by the RealTime1 flag
    // bit -- see this file's header comment.
    uint32_t &sequence = (flags & FlagRealTime1) ? m_sequenceRealTime : m_sequence;

    std::vector<uint8_t> pkt;
    pkt.insert(pkt.end(), SIGN, SIGN + 8);
    appendU16LE(pkt, type);
    appendU16LE(pkt, flags);
    appendU32LE(pkt, ++sequence);
    appendU16LE(pkt, static_cast<uint16_t>(payload.size()));
    pkt.insert(pkt.end(), payload.begin(), payload.end());
    ::send(m_fd, pkt.data(), pkt.size(), 0);
}

void RewindClient::sendKeepAlive() {
    // VersionData: RemoteID(u32 LE) + Service(u8) + Description (exactly
    // as many bytes as the string needs -- confirmed against
    // BrandMeister/DigestPlay's RewindClient.c, where
    // RewindVersionData::description is a flexible array member, not a
    // fixed/padded field).
    std::vector<uint8_t> payload;
    appendU32LE(payload, m_remoteId);
    payload.push_back(SERVICE_OPEN_TERMINAL);
    payload.insert(payload.end(), m_description.begin(), m_description.end());
    sendPacket(TypeKeepAlive, payload);
}

void RewindClient::sendConfiguration() {
    std::vector<uint8_t> payload;
    appendU32LE(payload, m_options);
    sendPacket(TypeConfiguration, payload);
}

void RewindClient::subscribe(uint32_t targetId, SessionType type) {
    std::vector<uint8_t> payload;
    appendU32LE(payload, static_cast<uint32_t>(type));
    appendU32LE(payload, targetId);
    sendPacket(TypeSubscription, payload);
    std::fprintf(stderr, "dmr_rewind: subscribed to %u (%s)\n", targetId,
                 type == SessionType::GroupVoice ? "group" : "private");
}

void RewindClient::disconnect() {
    if (m_fd < 0) return;
    sendPacket(TypeClose, {});
    ::close(m_fd);
    m_fd = -1;
}

void RewindClient::setSuperHeaderSink(std::function<void(const SuperHeaderInfo &)> sink) {
    m_superHeaderSink = std::move(sink);
}

void RewindClient::setDmrDataSink(std::function<void(uint8_t, const uint8_t *, size_t)> sink) {
    m_dmrDataSink = std::move(sink);
}

void RewindClient::setHeaderSink(std::function<void(uint32_t, uint32_t)> sink) { m_headerSink = std::move(sink); }

void RewindClient::setVoiceRxSink(std::function<void(const uint8_t *, const uint8_t *, const uint8_t *)> sink) {
    m_voiceRxSink = std::move(sink);
}

uint32_t RewindClient::beginVoiceTx(uint32_t dstId, dmr::CallType callType) {
    m_txDstId = dstId;
    m_txPrivateCall = callType == dmr::CallType::Private;
    m_txStreamCounter++;

    // FullLC content: FLCO(6 bits)+reserved(1)+protect(1), FID, Service
    // Options, DestinationID(3 BE), SourceID(3 BE), then 3 trailing bytes
    // -- confirmed live these are a real DMR CRC-like field on receive,
    // but both this client and BrandMeister's own DigestPlay reference
    // leave them zero on transmit. FLCO 0=Group Voice, 3=Unit-to-Unit
    // Voice (confirmed against go-brandmeister's own test vectors and
    // pyspot_rx's encode_flc, which uses these same raw values).
    std::vector<uint8_t> lc(12, 0);
    lc[0] = m_txPrivateCall ? 0x03 : 0x00;
    lc[3] = static_cast<uint8_t>(dstId >> 16);
    lc[4] = static_cast<uint8_t>(dstId >> 8);
    lc[5] = static_cast<uint8_t>(dstId);
    lc[6] = static_cast<uint8_t>(m_remoteId >> 16);
    lc[7] = static_cast<uint8_t>(m_remoteId >> 8);
    lc[8] = static_cast<uint8_t>(m_remoteId);

    // Sent 3 times for reliability, matching DigestPlay's own transmit.
    for (int i = 0; i < 3; i++) sendPacket(TypeDMRDataBase + DMR_DATA_VOICE_HEADER, lc, FlagRealTime1);
    std::fprintf(stderr, "dmr_rewind: PTT down, dst=%u, %s\n", dstId, m_txPrivateCall ? "private" : "group");
    return m_txStreamCounter;
}

void RewindClient::sendVoiceFrame(uint32_t /*streamId*/, const uint8_t ambe0[dmr::AMBE_FRAME_SIZE],
                                   const uint8_t ambe1[dmr::AMBE_FRAME_SIZE], const uint8_t ambe2[dmr::AMBE_FRAME_SIZE]) {
    std::vector<uint8_t> payload;
    payload.insert(payload.end(), ambe0, ambe0 + dmr::AMBE_FRAME_SIZE);
    payload.insert(payload.end(), ambe1, ambe1 + dmr::AMBE_FRAME_SIZE);
    payload.insert(payload.end(), ambe2, ambe2 + dmr::AMBE_FRAME_SIZE);
    sendPacket(TypeDMRAudioBase, payload, FlagRealTime1);
}

void RewindClient::endVoiceTx(uint32_t /*streamId*/) {
    // Empty payload -- matching DigestPlay's own terminator send exactly
    // (TransmitRewindData(..., REWIND_TYPE_DMR_DATA_BASE + 2, ..., NULL, 0)).
    sendPacket(TypeDMRDataBase + DMR_DATA_TERMINATOR_LC, {}, FlagRealTime1);
    std::fprintf(stderr, "dmr_rewind: PTT up\n");
}

LinkResult RewindClient::link() {
    // Matches BrandMeister's own reference client (DigestPlay's
    // RewindClient.c, ConnectRewindClient): KeepAlive is resent every
    // loop iteration -- not just up front -- until the whole handshake
    // completes, rather than being a one-shot "hello" followed by
    // silently waiting. What marks a connection authenticated differs
    // between the two real references read so far: the official C
    // client treats getting a plain KeepAlive back (instead of another
    // Challenge) as success and moves straight to subscribing, while
    // go-brandmeister's Go client waits for an explicit Configuration
    // packet instead. Accepting either here, since it's not yet certain
    // both are equivalent across every master.
    uint8_t buf[256];
    int authAttempts = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    while (g_running && std::chrono::steady_clock::now() < deadline) {
        sendKeepAlive();
        ssize_t n = recvWithTimeout(buf, sizeof(buf), 1000);
        if (n < static_cast<ssize_t>(HEADER_LENGTH) || std::memcmp(buf, SIGN, 8) != 0) continue;
        uint16_t type = readU16LE(buf + 8);

        if (type == TypeChallenge) {
            if (authAttempts >= 3) {
                std::fprintf(stderr, "dmr_rewind: too many failed authentication attempts (wrong password?)\n");
                return LinkResult::AuthRejected;
            }
            std::vector<uint8_t> hashInput(buf + HEADER_LENGTH, buf + n);
            hashInput.insert(hashInput.end(), m_password.begin(), m_password.end());
            uint8_t digest[32];
            dmr::sha256(hashInput.data(), hashInput.size(), digest);
            sendPacket(TypeAuthentication, std::vector<uint8_t>(digest, digest + 32));
            authAttempts++;
            std::fprintf(stderr, "dmr_rewind: sent authentication response (attempt %d)\n", authAttempts);
        } else if (type == TypeConfiguration || type == TypeKeepAlive) {
            m_authenticated = true;
            sendConfiguration(); // declare our own Options (SuperHeader, etc.)
            std::fprintf(stderr, "dmr_rewind: authenticated\n");
            return LinkResult::Success;
        } else if (type == TypeClose) {
            std::fprintf(stderr, "dmr_rewind: server closed the connection (wrong password?)\n");
            return LinkResult::AuthRejected;
        } else {
            std::fprintf(stderr, "dmr_rewind: unexpected reply type %#06x during handshake\n", type);
        }
    }

    std::fprintf(stderr, "dmr_rewind: handshake timed out\n");
    return LinkResult::Timeout;
}

void RewindClient::handlePacket(const uint8_t *data, size_t len) {
    if (len < HEADER_LENGTH || std::memcmp(data, SIGN, 8) != 0) return;
    uint16_t type = readU16LE(data + 8);
    const uint8_t *payload = data + HEADER_LENGTH;
    size_t payloadLen = len - HEADER_LENGTH;

    if (type == TypeClose) {
        std::fprintf(stderr, "dmr_rewind: server sent close\n");
        m_authenticated = false;
    } else if (type == TypeKeepAlive) {
        // Server's own heartbeat -- nothing to do.
    } else if (type == TypeConfiguration) {
        // Server's own Configuration echo -- nothing to do (Options are
        // declared once, right after link() succeeds).
    } else if (type == TypeSubscription) {
        // Confirms a subscribe()/unsubscribe() request went through --
        // fire-and-forget from this client's side, so just log it.
        std::fprintf(stderr, "dmr_rewind: subscription confirmed\n");
    } else if (type == TypeSuperHeader) {
        constexpr size_t SUPER_HEADER_LENGTH = 12 + 2 * CALL_LENGTH;
        if (payloadLen < SUPER_HEADER_LENGTH) {
            std::fprintf(stderr, "dmr_rewind: short SuperHeader (%zu bytes)\n", payloadLen);
            return;
        }
        SuperHeaderInfo info;
        info.sessionType = readU32LE(payload);
        info.source = readU32LE(payload + 4);
        info.target = readU32LE(payload + 8);
        info.sourceCall = callsignFromBytes(payload + 12, CALL_LENGTH);
        info.targetCall = callsignFromBytes(payload + 12 + CALL_LENGTH, CALL_LENGTH);
        if (m_superHeaderSink) m_superHeaderSink(info);
        // SuperHeader arrives once per transmission, unlike the DMRData
        // VoiceHeader (sent 3x) -- the better source for a "last heard"
        // signal that should fire exactly once per call.
        if (m_headerSink) m_headerSink(info.source, info.target);
    } else if (type >= TypeDMRDataBase && type < TypeDMRAudioBase) {
        uint8_t dataType = static_cast<uint8_t>(type & 0x0f);
        if (m_dmrDataSink) m_dmrDataSink(dataType, payload, payloadLen);
    } else if (type >= TypeDMRAudioBase && type < TypeDMREmbeddedData) {
        // 3 concatenated 9-byte AMBE half-rate frames -- confirmed live,
        // see this file's header comment. Anything else is unexpected
        // (e.g. OptionLinearFrame's different framing, not requested
        // here) -- logged, not guessed at.
        if (payloadLen == 3 * dmr::AMBE_FRAME_SIZE) {
            if (m_voiceRxSink)
                m_voiceRxSink(payload, payload + dmr::AMBE_FRAME_SIZE, payload + 2 * dmr::AMBE_FRAME_SIZE);
        } else {
            std::fprintf(stderr, "dmr_rewind: DMRAudio unexpected length %zu (expected %zu)\n", payloadLen,
                         3 * dmr::AMBE_FRAME_SIZE);
        }
    } else {
        std::fprintf(stderr, "dmr_rewind: unhandled packet type %#06x (%zu bytes)\n", type, payloadLen);
    }
}

void RewindClient::run() {
    auto lastKeepAlive = std::chrono::steady_clock::now();
    auto lastRecv = std::chrono::steady_clock::now();
    uint8_t buf[512];

    while (g_running) {
        auto now = std::chrono::steady_clock::now();
        if (m_authenticated && now - lastKeepAlive >= std::chrono::seconds(KEEPALIVE_INTERVAL_SEC)) {
            sendKeepAlive();
            lastKeepAlive = now;
        }
        if (now - lastRecv > std::chrono::seconds(TIMEOUT_SEC)) {
            std::fprintf(stderr, "dmr_rewind: timed out waiting for server\n");
            return;
        }

        ssize_t n = recvWithTimeout(buf, sizeof(buf), 200);
        if (n <= 0) continue;
        lastRecv = std::chrono::steady_clock::now();
        handlePacket(buf, static_cast<size_t>(n));
    }
}

} // namespace dmr::rewind
