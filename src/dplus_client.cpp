#include "dplus_client.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <thread>

#include "ccrc.h" // xlxd's CCITT CRC, the D-Star header's (via dmr_fec)

namespace dplus {

namespace {

constexpr const char *AUTH_HOST = "auth.dstargateway.org";
constexpr int AUTH_PORT = 20001;
constexpr int AUTH_TIMEOUT_SEC = 10;
constexpr int KEEPALIVE_PERIOD_MS = 1000; // ircDDBGateway's poll timer
constexpr int LINK_TIMEOUT_SEC = 30;      // ...and its poll inactivity timeout

const uint8_t CONNECT[] = {0x05, 0x00, 0x18, 0x00, 0x01};
const uint8_t DISCONNECT[] = {0x05, 0x00, 0x18, 0x00, 0x00};
const uint8_t KEEPALIVE[] = {0x03, 0x60, 0x00};

std::string trimmed(const uint8_t *p, size_t n) {
    std::string s(reinterpret_cast<const char *>(p), n);
    size_t end = s.find_last_not_of(" \0", std::string::npos, 2);
    return end == std::string::npos ? std::string() : s.substr(0, end + 1);
}

int connectSocket(const std::string &host, int port, int type) {
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = type;
    struct addrinfo *res = nullptr;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) return -1;
    int fd = ::socket(AF_INET, type, 0);
    if (fd >= 0 && ::connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

bool readExactly(int fd, uint8_t *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv{AUTH_TIMEOUT_SEC, 0};
        if (::select(fd + 1, &fds, nullptr, nullptr, &tv) <= 0) return false;
        ssize_t n = ::recv(fd, buf + got, len - got, 0);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

} // namespace

bool authenticate(const std::string &callsign, std::vector<GatewayEntry> *gateways, std::string &error) {
    int fd = connectSocket(AUTH_HOST, AUTH_PORT, SOCK_STREAM);
    if (fd < 0) {
        error = std::string("cannot connect to ") + AUTH_HOST;
        return false;
    }

    // Same 56-byte login as ircDDBGateway's DPlusAuthenticator: space-filled,
    // callsign at 4, then fixed fields every client sends.
    uint8_t login[56];
    std::memset(login, ' ', sizeof(login));
    login[0] = 0x38;
    login[1] = 0xC0;
    login[2] = 0x01;
    login[3] = 0x00;
    std::memcpy(login + 4, callsign.data(), std::min<size_t>(callsign.size(), 8));
    std::memcpy(login + 12, "DV019999", 8);
    std::memcpy(login + 28, "W7IB", 4);
    login[32] = '2';
    std::memcpy(login + 40, "DHS0257", 7);
    if (::send(fd, login, sizeof(login), 0) != static_cast<ssize_t>(sizeof(login))) {
        error = "cannot send login";
        ::close(fd);
        return false;
    }

    // Replies: 2-byte length (low 12 bits) then the rest; each lists
    // 26-byte entries from offset 8: address[16], name[8], 1 byte, flags
    // (0x80 = active). The server closes the connection when done.
    bool gotReply = false;
    uint8_t buf[4096];
    while (readExactly(fd, buf, 2)) {
        size_t len = (buf[1] & 0x0F) * 256u + buf[0];
        if (len < 2 || len > sizeof(buf) || !readExactly(fd, buf + 2, len - 2)) break;
        if ((buf[1] & 0xC0) != 0xC0 || buf[2] != 0x01) {
            error = "unexpected reply from the authentication server";
            break;
        }
        gotReply = true;
        for (size_t i = 8; i + 25 < len; i += 26) {
            std::string address = trimmed(buf + i, 16), name = trimmed(buf + i + 16, 8);
            bool active = (buf[i + 25] & 0x80) != 0;
            if (gateways && active && !address.empty() && !name.empty()) gateways->push_back({name, address});
        }
    }
    ::close(fd);
    if (!gotReply && error.empty()) error = "no reply from the authentication server";
    return gotReply;
}

DPlusClient::~DPlusClient() {
    if (m_fd >= 0) ::close(m_fd);
}

bool DPlusClient::open(const std::string &host) {
    m_fd = connectSocket(host, DPLUS_PORT, SOCK_DGRAM);
    if (m_fd < 0) {
        std::fprintf(stderr, "dplus: cannot open UDP socket to %s:%d\n", host.c_str(), DPLUS_PORT);
        return false;
    }
    std::fprintf(stderr, "dplus: opened UDP socket to %s:%d\n", host.c_str(), DPLUS_PORT);
    return true;
}

bool DPlusClient::link(const std::string &callsign) {
    // Connect request (echoed back), then the 28-byte login: callsign
    // zero-padded at 4, "DV019999" at 20 (ircDDBGateway's CT_LINK2).
    uint8_t login[28] = {0x1C, 0xC0, 0x04, 0x00};
    std::memcpy(login + 4, callsign.data(), std::min<size_t>(callsign.size(), 16));
    std::memcpy(login + 20, "DV019999", 8);

    bool connected = false;
    for (int attempt = 0; attempt < 5; attempt++) {
        ::send(m_fd, connected ? login : CONNECT, connected ? sizeof(login) : sizeof(CONNECT), 0);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            uint8_t buf[64];
            ssize_t n = recvWithTimeout(buf, sizeof(buf), 200);
            if (!connected && n == 5 && std::memcmp(buf, CONNECT, 5) == 0) {
                connected = true;
                std::fprintf(stderr, "dplus: connect acknowledged, logging in as %s\n", callsign.c_str());
                ::send(m_fd, login, sizeof(login), 0);
            } else if (connected && n == 8 && buf[0] == 0x08 && buf[1] == 0xC0) {
                if (std::memcmp(buf + 4, "OKRW", 4) == 0) {
                    std::fprintf(stderr, "dplus: login accepted\n");
                    return true;
                }
                // "OK, read-only": may listen but not transmit -- what
                // ZL2VH answers for a callsign suffix that isn't a
                // registered terminal (ZL2MIM D, while ZL2MIM and ZL2MIM B
                // got OKRW).
                if (std::memcmp(buf + 4, "OKRO", 4) == 0) {
                    std::fprintf(stderr, "dplus: login accepted read-only (listen only, transmitting not allowed)\n");
                    return true;
                }
                std::fprintf(stderr, "dplus: login refused (%.4s) -- is %s registered?\n", buf + 4, callsign.c_str());
                return false;
            }
        }
    }
    std::fprintf(stderr, "dplus: no answer to %s\n", connected ? "login" : "connect request");
    return false;
}

void DPlusClient::sendKerchunk(const std::string &gateway, char module, const std::string &callsign) {
    auto field = [](const std::string &s, size_t n) {
        std::string f = s.substr(0, n);
        f.resize(n, ' ');
        return f;
    };
    // "ZL2VH  C": the gateway callsign padded to 7, module in the 8th byte;
    // module first, gateway ("G") second -- see the header comment.
    std::string first = field(gateway, 7) + module, second = field(gateway, 7) + 'G';
    uint16_t streamId = static_cast<uint16_t>(std::chrono::steady_clock::now().time_since_epoch().count() | 1);

    uint8_t header[58] = {0x3A, 0x80, 'D', 'S', 'V', 'T', 0x10, 0x00, 0x00, 0x00, 0x20, 0x00, 0x01, 0x02};
    header[14] = static_cast<uint8_t>(streamId);
    header[15] = static_cast<uint8_t>(streamId >> 8);
    header[16] = 0x80;
    uint8_t *h = header + 17; // flags[3], RPT2, RPT1, UR, MY, MY2, CRC
    h[0] = h[1] = h[2] = 0x00;
    std::memcpy(h + 3, first.data(), 8);
    std::memcpy(h + 11, second.data(), 8);
    std::memcpy(h + 19, field("CQCQCQ", 8).data(), 8);
    std::memcpy(h + 27, field(callsign, 8).data(), 8);
    std::memcpy(h + 35, "    ", 4);
    CCRC::addCCITT161(h, 41); // fills h[39..40]
    for (int i = 0; i < 3; i++) ::send(m_fd, header, sizeof(header), 0);

    // D-Star AMBE silence; slow data carries the sync pattern on frame 0
    // and the idle filler otherwise.
    const uint8_t silence[9] = {0x9E, 0x8D, 0x32, 0x88, 0x26, 0x1A, 0x3F, 0x61, 0xE8};
    const uint8_t sync[3] = {0x55, 0x2D, 0x16}, filler[3] = {0x16, 0x29, 0xF5};
    uint8_t frame[29] = {0x1D, 0x80, 'D', 'S', 'V', 'T', 0x20, 0x00, 0x00, 0x00, 0x20, 0x00, 0x01, 0x02};
    frame[14] = header[14];
    frame[15] = header[15];
    uint8_t packetId = 0;
    for (; packetId < 10; packetId++) {
        frame[16] = packetId;
        std::memcpy(frame + 17, silence, 9);
        std::memcpy(frame + 26, packetId == 0 ? sync : filler, 3);
        ::send(m_fd, frame, sizeof(frame), 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // End of stream, as xlxd encodes it.
    uint8_t last[32] = {0x20, 0x80, 'D', 'S', 'V', 'T', 0x20, 0x00, 0x81, 0x00, 0x20, 0x00, 0x01, 0x02};
    last[14] = header[14];
    last[15] = header[15];
    last[16] = static_cast<uint8_t>(packetId | 0x40);
    const uint8_t endPattern[15] = {0x55, 0xC8, 0x7A, 0x55, 0x55, 0x55, 0x55, 0x55,
                                    0x55, 0x55, 0x55, 0x55, 0x25, 0x1A, 0xC6};
    std::memcpy(last + 17, endPattern, 15);
    ::send(m_fd, last, sizeof(last), 0);
    std::fprintf(stderr, "dplus: sent a 0.2 s silent transmission to %s / %s (stream %u)\n", first.c_str(),
                 second.c_str(), streamId);
}

void DPlusClient::disconnect() {
    if (m_fd < 0) return;
    for (int i = 0; i < 3; i++) ::send(m_fd, DISCONNECT, sizeof(DISCONNECT), 0);
    std::fprintf(stderr, "dplus: sent disconnect\n");
}

ssize_t DPlusClient::recvWithTimeout(uint8_t *buf, size_t len, int timeoutMs) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(m_fd, &fds);
    struct timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    if (::select(m_fd + 1, &fds, nullptr, nullptr, &tv) <= 0) return 0;
    return ::recv(m_fd, buf, len, 0);
}

void DPlusClient::run(const volatile std::sig_atomic_t *running) {
    auto lastKeepalive = std::chrono::steady_clock::now();
    auto lastHeard = lastKeepalive;
    while (*running) {
        auto now = std::chrono::steady_clock::now();
        if (now - lastKeepalive >= std::chrono::milliseconds(KEEPALIVE_PERIOD_MS)) {
            ::send(m_fd, KEEPALIVE, sizeof(KEEPALIVE), 0);
            lastKeepalive = now;
        }
        if (now - lastHeard > std::chrono::seconds(LINK_TIMEOUT_SEC)) {
            std::fprintf(stderr, "dplus: nothing from the server for %d s, giving up\n", LINK_TIMEOUT_SEC);
            return;
        }
        uint8_t buf[128];
        ssize_t n = recvWithTimeout(buf, sizeof(buf), 200);
        if (n <= 0) continue;
        lastHeard = std::chrono::steady_clock::now();
        handlePacket(buf, static_cast<size_t>(n));
    }
}

void DPlusClient::handlePacket(const uint8_t *buf, size_t len) {
    bool dsvt = len >= 17 && std::memcmp(buf + 2, "DSVT", 4) == 0;
    uint16_t streamId = dsvt ? static_cast<uint16_t>(buf[14] | (buf[15] << 8)) : 0;

    if (len == 58 && buf[0] == 0x3A && buf[1] == 0x80 && dsvt && buf[6] == 0x10) {
        // Headers are repeated for reliability: pass each stream's on once.
        if (std::find(m_recentHeaders.begin(), m_recentHeaders.end(), streamId) != m_recentHeaders.end()) return;
        m_recentHeaders.push_back(streamId);
        if (m_recentHeaders.size() > 16) m_recentHeaders.erase(m_recentHeaders.begin());
        // dstar_header at 17: flags[3], RPT2[8], RPT1[8], UR[8], MY[8], MY2[4], CRC[2]
        dextra::DStarHeader header;
        header.rpt2 = trimmed(buf + 20, 8);
        header.rpt1 = trimmed(buf + 28, 8);
        header.urCall = trimmed(buf + 36, 8);
        header.myCall = trimmed(buf + 44, 8);
        header.myCall2 = trimmed(buf + 52, 4);
        if (m_headerSink) m_headerSink(streamId, header);
    } else if (len == 29 && buf[0] == 0x1D && buf[1] == 0x80 && dsvt) {
        if (m_frameSink) m_frameSink(streamId, buf[16], buf + 17, false);
    } else if (len == 32 && buf[0] == 0x20 && buf[1] == 0x80 && dsvt) {
        if (m_frameSink) m_frameSink(streamId, buf[16], nullptr, true);
    } else if (len == 3 && std::memcmp(buf, KEEPALIVE, 3) == 0) {
        // the server's own keepalive
    } else if (len == 5 && std::memcmp(buf, DISCONNECT, 5) == 0) {
        std::fprintf(stderr, "dplus: server disconnected us\n");
    } else {
        std::fprintf(stderr, "dplus: unrecognised packet (%zu bytes):", len);
        for (size_t i = 0; i < len && i < 64; i++) std::fprintf(stderr, " %02x", buf[i]);
        std::fprintf(stderr, "  \"");
        for (size_t i = 0; i < len && i < 64; i++) std::fputc(buf[i] >= 32 && buf[i] < 127 ? buf[i] : '.', stderr);
        std::fprintf(stderr, "\"\n");
    }
}

} // namespace dplus
