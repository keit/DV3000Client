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
#include "dextra_client.h" // dextra::g_running
#include "dstar_slowdata.h"

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

char streamModule(const dextra::DStarHeader &header) {
    // Fields are trimmed, so a module letter is only there at full length.
    auto module = [](const std::string &rpt) { return rpt.size() == 8 ? rpt[7] : ' '; };
    char m2 = module(header.rpt2);
    return m2 != 'G' && m2 != ' ' ? m2 : module(header.rpt1);
}

DPlusClient::~DPlusClient() {
    if (m_fd >= 0) ::close(m_fd);
}

void DPlusClient::setIdentity(const std::string &callsign, char suffix, const std::string &myCall2) {
    m_callsign = callsign.substr(0, 7);
    m_loginCallsign = m_callsign;
    if (suffix != ' ' && suffix != '\0') {
        // "ZL2MIM B": callsign padded to 7, the letter 8th -- as
        // ircDDBGateway/Pi-Star hotspots log in.
        m_loginCallsign.resize(7, ' ');
        m_loginCallsign += suffix;
    }
    m_myCall2 = myCall2.substr(0, 4);
}

void DPlusClient::setTarget(const std::string &name, char module) {
    m_target = name.substr(0, 7);
    m_module = module;
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

LinkResult DPlusClient::link() {
    // Connect request (echoed back), then the 28-byte login: callsign
    // zero-padded at 4, "DV019999" at 20 (ircDDBGateway's CT_LINK2).
    uint8_t login[28] = {0x1C, 0xC0, 0x04, 0x00};
    std::memcpy(login + 4, m_loginCallsign.data(), std::min<size_t>(m_loginCallsign.size(), 16));
    std::memcpy(login + 20, "DV019999", 8);

    bool connected = false;
    for (int attempt = 0; attempt < 5 && dextra::g_running; attempt++) {
        ::send(m_fd, connected ? login : CONNECT, connected ? sizeof(login) : sizeof(CONNECT), 0);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline && dextra::g_running) {
            uint8_t buf[64];
            ssize_t n = recvWithTimeout(buf, sizeof(buf), 200);
            if (!connected && n == 5 && std::memcmp(buf, CONNECT, 5) == 0) {
                connected = true;
                std::fprintf(stderr, "dplus: connect acknowledged, logging in as %s\n", m_loginCallsign.c_str());
                ::send(m_fd, login, sizeof(login), 0);
            } else if (connected && n == 8 && buf[0] == 0x08 && buf[1] == 0xC0) {
                m_loginReply.assign(reinterpret_cast<const char *>(buf + 4), 4);
                if (m_loginReply == "OKRW") {
                    std::fprintf(stderr, "dplus: login accepted\n");
                    return LinkResult::Accepted;
                }
                // What ZL2VH answers for a callsign suffix that isn't a
                // registered terminal (ZL2MIM D, while ZL2MIM and ZL2MIM B
                // got OKRW).
                if (m_loginReply == "OKRO") {
                    std::fprintf(stderr, "dplus: login accepted read-only (listen only, transmitting not allowed)\n");
                    return LinkResult::ReadOnly;
                }
                // BUSY: this login is already connected (the operator's
                // Repeater Monitor, hotspot link, ...).
                std::fprintf(stderr, "dplus: login refused (%s)\n", m_loginReply.c_str());
                return LinkResult::Refused;
            }
        }
    }
    std::fprintf(stderr, "dplus: no answer to %s\n", connected ? "login" : "connect request");
    return LinkResult::NoAnswer;
}

void DPlusClient::disconnect() {
    if (m_fd < 0) return;
    for (int i = 0; i < 3; i++) ::send(m_fd, DISCONNECT, sizeof(DISCONNECT), 0);
    std::fprintf(stderr, "dplus: sent disconnect\n");
}

// --- transmit ---------------------------------------------------------------

namespace {

std::string field(const std::string &s, size_t n) {
    std::string f = s.substr(0, n);
    f.resize(n, ' ');
    return f;
}

// D-Star AMBE silence.
const uint8_t SILENCE[9] = {0x9E, 0x8D, 0x32, 0x88, 0x26, 0x1A, 0x3F, 0x61, 0xE8};

} // namespace

void DPlusClient::sendHeader(uint16_t streamId) {
    // Routed the way Pi-Star/ircDDBGateway send a hotspot's traffic: RPT2
    // (first on the wire) the target and module, "ZL2VH  C"; RPT1 our own
    // login, "ZL2MIM P" -- xlxd's DPlus reads the client's module from RPT1
    // and routes on RPT2.
    uint8_t header[58] = {0x3A, 0x80, 'D', 'S', 'V', 'T', 0x10, 0x00, 0x00, 0x00, 0x20, 0x00, 0x01, 0x02};
    header[14] = static_cast<uint8_t>(streamId);
    header[15] = static_cast<uint8_t>(streamId >> 8);
    header[16] = 0x80;
    uint8_t *h = header + 17; // flags[3], RPT2, RPT1, UR, MY, MY2, CRC
    h[0] = h[1] = h[2] = 0x00;
    std::memcpy(h + 3, (field(m_target, 7) + m_module).data(), 8);
    std::memcpy(h + 11, field(m_loginCallsign, 8).data(), 8);
    std::memcpy(h + 19, field("CQCQCQ", 8).data(), 8);
    // MY too: the terminal letter in MY's 8th byte is how a station shows
    // which of its terminals is talking (dashboards' Last Heard, callsign
    // routing to that terminal), e.g. "ZL2TWS P".
    std::memcpy(h + 27, field(m_loginCallsign, 8).data(), 8);
    std::memcpy(h + 35, field(m_myCall2, 4).data(), 4);
    CCRC::addCCITT161(h, 41); // fills h[39..40]
    for (int i = 0; i < 3; i++) ::send(m_fd, header, sizeof(header), 0);
    std::fprintf(stderr, "dplus: TX stream %u %s\n", streamId, dstar::describeTxHeader(h + 3, m_txMessage).c_str());
}

void DPlusClient::sendFrame(uint16_t streamId, uint8_t packetId, const uint8_t *ambe) {
    uint8_t frame[29] = {0x1D, 0x80, 'D', 'S', 'V', 'T', 0x20, 0x00, 0x00, 0x00, 0x20, 0x00, 0x01, 0x02};
    frame[14] = static_cast<uint8_t>(streamId);
    frame[15] = static_cast<uint8_t>(streamId >> 8);
    frame[16] = packetId;
    std::memcpy(frame + 17, ambe, 9);
    dstar::slowData(packetId, m_txMessage, frame + 26);
    ::send(m_fd, frame, sizeof(frame), 0);
}

void DPlusClient::sendLast(uint16_t streamId, uint8_t packetId) {
    // End of stream, as xlxd encodes it.
    uint8_t last[32] = {0x20, 0x80, 'D', 'S', 'V', 'T', 0x20, 0x00, 0x81, 0x00, 0x20, 0x00, 0x01, 0x02};
    last[14] = static_cast<uint8_t>(streamId);
    last[15] = static_cast<uint8_t>(streamId >> 8);
    last[16] = static_cast<uint8_t>(packetId | 0x40);
    const uint8_t endPattern[15] = {0x55, 0xC8, 0x7A, 0x55, 0x55, 0x55, 0x55, 0x55,
                                    0x55, 0x55, 0x55, 0x55, 0x25, 0x1A, 0xC6};
    std::memcpy(last + 17, endPattern, 15);
    ::send(m_fd, last, sizeof(last), 0);
}

uint16_t DPlusClient::beginLiveTx() {
    uint16_t streamId = static_cast<uint16_t>(std::chrono::steady_clock::now().time_since_epoch().count() | 1);
    m_txPacketId = 0;
    sendHeader(streamId);
    std::fprintf(stderr, "dplus: PTT down, stream %u\n", streamId);
    return streamId;
}

void DPlusClient::sendLiveTxFrame(uint16_t streamId, const uint8_t *ambe) {
    sendFrame(streamId, m_txPacketId, ambe);
    m_txPacketId = static_cast<uint8_t>((m_txPacketId + 1) % 21);
}

void DPlusClient::endLiveTx(uint16_t streamId) {
    sendLast(streamId, m_txPacketId);
    m_txPacketId = 0;
    std::fprintf(stderr, "dplus: PTT up, stream %u\n", streamId);
}

void DPlusClient::sendKerchunk() {
    uint16_t streamId = beginLiveTx();
    for (int i = 0; i < 10; i++) {
        sendLiveTxFrame(streamId, SILENCE);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    endLiveTx(streamId);
}

// --- receive ----------------------------------------------------------------

ssize_t DPlusClient::recvWithTimeout(uint8_t *buf, size_t len, int timeoutMs) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(m_fd, &fds);
    struct timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    if (::select(m_fd + 1, &fds, nullptr, nullptr, &tv) <= 0) return 0;
    return ::recv(m_fd, buf, len, 0);
}

void DPlusClient::run() { run(&dextra::g_running); }

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
        expireStreams();
        if (n <= 0) continue;
        lastHeard = std::chrono::steady_clock::now();
        handlePacket(buf, static_cast<size_t>(n));
    }
}

void DPlusClient::endStream(uint16_t streamId, const char *how) {
    auto it = m_rxStreams.find(streamId);
    if (it == m_rxStreams.end()) return;
    const RxStream &s = it->second;
    double secs = std::chrono::duration<double>(s.last - s.start).count();
    if (s.frames == 0) {
        std::fprintf(stderr, "dplus: RX stream %u %s on %c %s: header only, no audio, over %.1f s\n", streamId,
                     s.who.c_str(), s.module, how, secs);
    } else {
        std::fprintf(stderr, "dplus: RX stream %u %s on %c %s: %d frames (%.1f s of audio) over %.1f s\n", streamId,
                     s.who.c_str(), s.module, how, s.frames, s.frames * 0.02, secs);
    }
    m_rxStreams.erase(it);
}

void DPlusClient::expireStreams() {
    auto now = std::chrono::steady_clock::now();
    for (auto it = m_rxStreams.begin(); it != m_rxStreams.end();) {
        uint16_t id = (it++)->first; // endStream erases it
        if (now - m_rxStreams[id].last > std::chrono::seconds(2)) endStream(id, "timed out");
    }
}

void DPlusClient::onHeader(uint16_t streamId, const dextra::DStarHeader &header) {
    auto now = std::chrono::steady_clock::now();
    // A played stream that went quiet without its last frame is over.
    if (m_playing >= 0 && now - m_playingLast > std::chrono::seconds(1)) m_playing = -1;
    char module = streamModule(header);
    const char *played = "playing";
    if (m_playing < 0 && (m_module == '*' || module == m_module)) {
        m_playing = streamId;
        m_playingLast = now;
    } else {
        played = m_playing >= 0 ? "not played: another stream is playing" : "not played: other module";
    }
    std::string who = header.myCall + (header.myCall2.empty() ? "" : "/" + header.myCall2);
    RxStream &rx = m_rxStreams[streamId];
    rx = RxStream{who, module == ' ' ? '?' : module, 0, now, now, header, {}};
    rx.slowData.reset();
    std::fprintf(stderr, "dplus: RX stream %u %s on %c started (UR %s, RPT1 %s, RPT2 %s; %s)\n", streamId, who.c_str(),
                 module == ' ' ? '?' : module, header.urCall.c_str(), header.rpt1.c_str(), header.rpt2.c_str(), played);
    if (m_rawHeaderSink) m_rawHeaderSink(streamId, header);
    if (m_headerSink) m_headerSink(header);
}

void DPlusClient::onFrame(uint16_t streamId, uint8_t packetId, const uint8_t *ambe, bool last) {
    if (m_rawFrameSink) m_rawFrameSink(streamId, packetId, ambe, last);
    auto rx = m_rxStreams.find(streamId);
    if (rx != m_rxStreams.end()) {
        rx->second.last = std::chrono::steady_clock::now();
        if (!last) {
            rx->second.frames++;
            // The slow data follows the 9 AMBE bytes.
            if (rx->second.slowData.add(packetId, ambe + 9)) {
                std::string message = rx->second.slowData.message();
                std::fprintf(stderr, "dplus: RX stream %u %s TX message \"%s\"\n", streamId, rx->second.who.c_str(),
                             message.c_str());
                if (m_txMessageSink && !message.empty()) m_txMessageSink(rx->second.header, message);
            }
        }
    }
    if (last) endStream(streamId, "ended");
    if (streamId != m_playing) return;
    m_playingLast = std::chrono::steady_clock::now();
    if (last) {
        m_playing = -1;
    } else if (m_vocoder && m_liveRxSink) {
        m_vocoder->decode(ambe, [this](const short *pcm) { m_liveRxSink(pcm); });
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
        onHeader(streamId, header);
    } else if (len == 29 && buf[0] == 0x1D && buf[1] == 0x80 && dsvt) {
        onFrame(streamId, buf[16], buf + 17, false);
    } else if (len == 32 && buf[0] == 0x20 && buf[1] == 0x80 && dsvt) {
        onFrame(streamId, buf[16], nullptr, true);
    } else if (len == 10 && buf[0] == 0x0A && buf[1] == 0xC0 && buf[2] == 0x0B) {
        // A DPlus gateway's end-of-stream summary: stream ID, packet count.
        // Its RF users' streams end with this rather than a last frame.
        uint16_t id = static_cast<uint16_t>(buf[4] | (buf[5] << 8));
        if (id == m_playing) m_playing = -1;
        endStream(id, "ended (gateway summary)");
    } else if (len == 24 && buf[0] == 0x18 && buf[1] == 0xC0 && buf[2] == 0x03) {
        // A gateway's last-heard entry (callsign, repeater, time) -- the
        // headers already tell us.
    } else if (len > 10 && buf[1] == 0xC0 && buf[2] == 0x10 && std::memcmp(buf + 4, "gwmsg=", 6) == 0) {
        std::fprintf(stderr, "dplus: gateway says: %s\n", trimmed(buf + 10, len - 10).c_str());
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
