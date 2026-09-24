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

void RewindClient::sendPacket(uint16_t type, const std::vector<uint8_t> &payload) {
    std::vector<uint8_t> pkt;
    pkt.insert(pkt.end(), SIGN, SIGN + 8);
    appendU16LE(pkt, type);
    appendU16LE(pkt, 0); // flags -- FlagDefaultSet (0) for everything we send
    appendU32LE(pkt, ++m_sequence);
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

void RewindClient::setDmrAudioSink(std::function<void(uint8_t, const uint8_t *, size_t)> sink) {
    m_dmrAudioSink = std::move(sink);
}

bool RewindClient::link() {
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
                return false;
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
            return true;
        } else if (type == TypeClose) {
            std::fprintf(stderr, "dmr_rewind: server closed the connection (wrong password?)\n");
            return false;
        } else {
            std::fprintf(stderr, "dmr_rewind: unexpected reply type %#06x during handshake\n", type);
        }
    }

    std::fprintf(stderr, "dmr_rewind: handshake timed out\n");
    return false;
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
    } else if (type >= TypeDMRDataBase && type < TypeDMRAudioBase) {
        uint8_t dataType = static_cast<uint8_t>(type & 0x0f);
        if (m_dmrDataSink) m_dmrDataSink(dataType, payload, payloadLen);
    } else if (type >= TypeDMRAudioBase && type < TypeDMREmbeddedData) {
        uint8_t subType = static_cast<uint8_t>(type - TypeDMRAudioBase);
        if (m_dmrAudioSink) m_dmrAudioSink(subType, payload, payloadLen);
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
