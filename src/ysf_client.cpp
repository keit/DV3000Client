#include "ysf_client.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cysffich.h"
#include "main.h" // ysf_fec's stand-in for xlxd's: the uint8 cysfutils.h uses
#include "cysfutils.h"
#include "http_get.h"
#include "ysfdefines.h"

namespace ysf {

namespace {

constexpr const char *HOSTS_HOST = "www.pistar.uk";
constexpr const char *HOSTS_PATH = "/downloads/YSF_Hosts.txt";
constexpr int POLL_PERIOD_SEC = 5;    // YSFGateway's
constexpr int LINK_TIMEOUT_SEC = 30;  // nothing at all from the reflector
constexpr int STREAM_TIMEOUT_MS = 1500; // a stream that stopped without its end flag

constexpr size_t CALLSIGN_FIELD = 10;
constexpr size_t DATA_PACKET_SIZE = 155; // "YSFD" + 3 callsigns + counter + 120-byte frame
constexpr size_t FRAME_OFFSET = 35;

std::string trimmed(const uint8_t *p, size_t n) {
    std::string s(reinterpret_cast<const char *>(p), n);
    size_t end = s.find_last_not_of(" \0", std::string::npos, 2);
    return end == std::string::npos ? std::string() : s.substr(0, end + 1);
}

std::string field(const std::string &s, size_t n) {
    std::string f = s.substr(0, n);
    f.resize(n, ' ');
    return f;
}

const char *dataTypeName(int dt) {
    switch (dt) {
    case YSF_DT_VD_MODE2: return "DN";
    case YSF_DT_VOICE_FR_MODE: return "VW";
    case YSF_DT_VD_MODE1: return "V/D mode 1";
    case YSF_DT_DATA_FR_MODE: return "data";
    default: return "?";
    }
}

} // namespace

std::vector<ReflectorEntry> parseReflectorList(const std::string &text) {
    // id;name;description;address;port;count;  -- '#' lines are comments
    std::vector<ReflectorEntry> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> f;
        std::stringstream ls(line);
        std::string item;
        while (std::getline(ls, item, ';')) f.push_back(item);
        if (f.size() < 5 || f[1].empty() || f[3].empty()) continue;
        ReflectorEntry r;
        r.id = f[0];
        r.name = f[1];
        r.description = f[2];
        r.host = f[3];
        r.port = std::atoi(f[4].c_str());
        if (r.port <= 0) r.port = DEFAULT_PORT;
        out.push_back(r);
    }
    return out;
}

bool fetchReflectorList(std::vector<ReflectorEntry> &out, std::string &error) {
    std::string body;
    if (!httpGetRaw(HOSTS_HOST, HOSTS_PATH, 80, body, error)) return false;
    out = parseReflectorList(body);
    if (out.empty()) {
        error = "no reflectors in the list from " + std::string(HOSTS_HOST);
        return false;
    }
    return true;
}

YsfClient::~YsfClient() {
    if (m_fd >= 0) ::close(m_fd);
}

bool YsfClient::open(const std::string &host, int port) {
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *res = nullptr;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        std::fprintf(stderr, "ysf: cannot resolve %s\n", host.c_str());
        return false;
    }
    m_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (m_fd >= 0 && ::connect(m_fd, res->ai_addr, res->ai_addrlen) < 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    freeaddrinfo(res);
    if (m_fd < 0) {
        std::fprintf(stderr, "ysf: cannot open UDP socket to %s:%d\n", host.c_str(), port);
        return false;
    }
    std::fprintf(stderr, "ysf: opened UDP socket to %s:%d\n", host.c_str(), port);
    return true;
}

void YsfClient::sendPoll() {
    std::string pkt = "YSFP" + field(m_callsign, CALLSIGN_FIELD);
    ::send(m_fd, pkt.data(), pkt.size(), 0);
}

bool YsfClient::link(const std::string &callsign) {
    m_callsign = callsign;
    for (int attempt = 0; attempt < 3; attempt++) {
        sendPoll();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            uint8_t buf[256];
            ssize_t n = recvWithTimeout(buf, sizeof(buf), 200);
            if (n >= 4 && std::memcmp(buf, "YSFP", 4) == 0) {
                std::fprintf(stderr, "ysf: linked as %s (reflector answered \"%s\")\n", callsign.c_str(),
                             trimmed(buf + 4, static_cast<size_t>(n) - 4).c_str());
                return true;
            }
        }
    }
    std::fprintf(stderr, "ysf: no answer from the reflector\n");
    return false;
}

void YsfClient::disconnect() {
    if (m_fd < 0) return;
    std::string pkt = "YSFU" + field(m_callsign, CALLSIGN_FIELD);
    for (int i = 0; i < 3; i++) ::send(m_fd, pkt.data(), pkt.size(), 0);
    std::fprintf(stderr, "ysf: sent unlink\n");
}

ssize_t YsfClient::recvWithTimeout(uint8_t *buf, size_t len, int timeoutMs) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(m_fd, &fds);
    struct timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    if (::select(m_fd + 1, &fds, nullptr, nullptr, &tv) <= 0) return 0;
    return ::recv(m_fd, buf, len, 0);
}

void YsfClient::run(const volatile std::sig_atomic_t *running) {
    auto lastPoll = std::chrono::steady_clock::now();
    auto lastHeard = lastPoll;
    while (*running) {
        auto now = std::chrono::steady_clock::now();
        if (now - lastPoll >= std::chrono::seconds(POLL_PERIOD_SEC)) {
            sendPoll();
            lastPoll = now;
        }
        if (now - lastHeard > std::chrono::seconds(LINK_TIMEOUT_SEC)) {
            std::fprintf(stderr, "ysf: nothing from the reflector for %d s, giving up\n", LINK_TIMEOUT_SEC);
            return;
        }
        if (m_inStream && now - m_lastData > std::chrono::milliseconds(STREAM_TIMEOUT_MS)) endStream("timed out");

        uint8_t buf[256];
        ssize_t n = recvWithTimeout(buf, sizeof(buf), 200);
        if (n <= 0) continue;
        lastHeard = std::chrono::steady_clock::now();
        if (n == static_cast<ssize_t>(DATA_PACKET_SIZE) && std::memcmp(buf, "YSFD", 4) == 0) {
            handleData(buf);
        } else if (n >= 4 && std::memcmp(buf, "YSFP", 4) == 0) {
            // the reflector's answer to our poll
        } else {
            std::fprintf(stderr, "ysf: unrecognised packet (%zd bytes): %.4s\n", n, reinterpret_cast<char *>(buf));
        }
    }
}

void YsfClient::handleData(const uint8_t *buf) {
    auto now = std::chrono::steady_clock::now();
    std::string gateway = trimmed(buf + 4, CALLSIGN_FIELD);
    std::string source = trimmed(buf + 14, CALLSIGN_FIELD);
    std::string destination = trimmed(buf + 24, CALLSIGN_FIELD);
    bool end = (buf[34] & 0x01) != 0;
    const uint8_t *frame = buf + FRAME_OFFSET;

    // A different talker, or the same one after a pause, is a new stream.
    if (m_inStream && (source != m_stream.source || gateway != m_stream.gateway)) endStream("cut off by another");
    if (!m_inStream) {
        m_inStream = true;
        m_stream = StreamInfo{source, destination, gateway, -1};
        m_voiceFrames = m_frames = 0;
        m_streamStart = now;
    }
    m_lastData = now;
    m_frames++;

    CYSFFICH fich;
    bool fichOk = fich.decode(frame + YSF_SYNC_LENGTH_BYTES);
    if (fichOk) {
        bool firstFich = m_stream.dataType < 0;
        m_stream.dataType = fich.getDT();
        if (firstFich) {
            std::fprintf(stderr, "ysf: RX stream %s -> %s via %s started (%s)\n", m_stream.source.c_str(),
                         m_stream.destination.c_str(), m_stream.gateway.c_str(), dataTypeName(m_stream.dataType));
            if (m_startSink) m_startSink(m_stream);
        }
        if (fich.getFI() == YSF_FI_COMMUNICATIONS && fich.getDT() == YSF_DT_VD_MODE2) {
            uint8_t ambe[5][AMBE_FRAME_SIZE];
            uint8_t *ptrs[5] = {ambe[0], ambe[1], ambe[2], ambe[3], ambe[4]};
            CYsfUtils::DecodeVD2Vchs(const_cast<uint8_t *>(frame), ptrs);
            for (auto &a : ambe) {
                m_voiceFrames++;
                if (m_voiceSink) m_voiceSink(a);
            }
        }
        if (fich.getFI() == YSF_FI_TERMINATOR) end = true;
    }
    if (end) endStream("ended");
}

void YsfClient::endStream(const char *how) {
    if (!m_inStream) return;
    m_inStream = false;
    double secs = std::chrono::duration<double>(m_lastData - m_streamStart).count();
    std::fprintf(stderr, "ysf: RX stream %s -> %s via %s %s (%s): %d frames, %d voice frames (%.1f s of audio) over %.1f s\n",
                 m_stream.source.c_str(), m_stream.destination.c_str(), m_stream.gateway.c_str(), how,
                 dataTypeName(m_stream.dataType), m_frames, m_voiceFrames, m_voiceFrames * 0.02, secs);
    if (m_endSink) m_endSink(m_stream, m_voiceFrames);
}

} // namespace ysf
