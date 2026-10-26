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
#include "ysf_dch.h"
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
    sendOptions();
}

void YsfClient::sendOptions() {
    if (m_options.empty()) return;
    // "YSFO", the callsign, then the options text padded to 50 bytes in all.
    std::string pkt = "YSFO" + field(m_callsign, CALLSIGN_FIELD) + field(m_options, 50 - 4 - CALLSIGN_FIELD);
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
                sendOptions();
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
        } else if (n >= 4 && (std::memcmp(buf, "YSFO", 4) == 0 || std::memcmp(buf, "YSFI", 4) == 0)) {
            std::fprintf(stderr, "ysf: reflector sent %.4s: \"%s\"\n", reinterpret_cast<char *>(buf),
                         trimmed(buf + 4, static_cast<size_t>(n) - 4).c_str());
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
        m_stream.dgId = fich.getSQ();
        if (firstFich) {
            std::fprintf(stderr, "ysf: RX stream %s -> %s via %s started (%s, DG-ID %02d)\n", m_stream.source.c_str(),
                         m_stream.destination.c_str(), m_stream.gateway.c_str(), dataTypeName(m_stream.dataType),
                         m_stream.dgId);
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

// --- transmit ----------------------------------------------------------------

void YsfClient::buildFich(uint8_t *frame, unsigned char fi, unsigned char fn) {
    // As Fusion radios send DN mode: frames numbered 0-6 (FT 6), CS 2, MR 0.
    std::memcpy(frame, YSF_SYNC_BYTES, YSF_SYNC_LENGTH_BYTES);
    CYSFFICH fich;
    fich.setFI(fi);
    fich.setCS(2);
    fich.setFN(fn);
    fich.setFT(6);
    fich.setDev(false);
    fich.setMR(0);
    fich.setDT(YSF_DT_VD_MODE2);
    fich.setSQL(false);
    fich.setSQ(static_cast<unsigned char>(m_dgId.load()));
    fich.encode(frame + YSF_SYNC_LENGTH_BYTES);
}

void YsfClient::sendFrame(const uint8_t *frame, bool last) {
    // gateway, source, destination; the counter (shifted left), bit 0 = end.
    std::string pkt = "YSFD" + field(m_callsign, CALLSIGN_FIELD) + field(m_callsign, CALLSIGN_FIELD) +
                      field("ALL", CALLSIGN_FIELD);
    pkt += static_cast<char>(((m_txCounter & 0x7F) << 1) | (last ? 0x01 : 0x00));
    pkt.append(reinterpret_cast<const char *>(frame), YSF_FRAME_LENGTH_BYTES);
    ::send(m_fd, pkt.data(), pkt.size(), 0);
    m_txCounter++;
}

namespace {

// Header and terminator: CSD1 = destination + source, CSD2 = downlink +
// uplink (left blank, as xlxd does).
void headerFields(const std::string &callsign, uint8_t csd1[CSD_BYTES], uint8_t csd2[CSD_BYTES]) {
    std::string c1 = "**********" + field(callsign, CALLSIGN_FIELD), c2(CSD_BYTES, ' ');
    std::memcpy(csd1, c1.data(), CSD_BYTES);
    std::memcpy(csd2, c2.data(), CSD_BYTES);
}

// A DN frame's data channel by frame number: 0 destination, 1 source,
// 2 downlink, 3 uplink, 4-6 radio data. As in xlxd, frames 5 and 6 carry a
// stand-in radio ID and GPS record, without which some radios don't show
// the caller's callsign.
void frameData(const std::string &callsign, unsigned char fn, uint8_t out[VD2_DATA_BYTES]) {
    static const uint8_t gps[VD2_DATA_BYTES] = {0x52, 0x22, 0x61, 0x5F, 0x27, 0x03, 0x5E, 0x20, 0x20, 0x20};
    std::string s;
    switch (fn) {
    case 0: s = "**********"; break;
    case 1: case 2: case 3: s = field(callsign, CALLSIGN_FIELD); break;
    case 5: s = "     G0gBJ"; break;
    case 6: std::memcpy(out, gps, VD2_DATA_BYTES); return;
    default: s = std::string(VD2_DATA_BYTES, ' '); break;
    }
    std::memcpy(out, s.data(), VD2_DATA_BYTES);
}

} // namespace

void YsfClient::beginTx() {
    m_txCount = 0;
    m_txFn = 0;
    m_txCounter = 0;
    uint8_t frame[YSF_FRAME_LENGTH_BYTES] = {};
    buildFich(frame, YSF_FI_HEADER, 0);
    uint8_t csd1[CSD_BYTES], csd2[CSD_BYTES];
    headerFields(m_callsign, csd1, csd2);
    encodeHeaderData(csd1, csd2, frame);
    sendFrame(frame, false);
    std::fprintf(stderr, "ysf: PTT down, %s, DG-ID %02d\n", m_callsign.c_str(), m_dgId.load());
}

void YsfClient::sendVoice(const uint8_t *ambe) {
    std::memcpy(m_txAmbe[m_txCount++], ambe, AMBE_FRAME_SIZE);
    if (m_txCount < 5) return;
    m_txCount = 0;
    uint8_t frame[YSF_FRAME_LENGTH_BYTES] = {};
    buildFich(frame, YSF_FI_COMMUNICATIONS, m_txFn);
    for (int i = 0; i < 5; i++)
        CYsfUtils::EncodeVD2Vch(m_txAmbe[i], frame + YSF_SYNC_LENGTH_BYTES + YSF_FICH_LENGTH_BYTES + 18 * i + 5);
    uint8_t data[VD2_DATA_BYTES];
    frameData(m_callsign, m_txFn, data);
    encodeVD2Data(data, frame);
    sendFrame(frame, false);
    m_txFn = static_cast<unsigned char>((m_txFn + 1) % 7);
}

void YsfClient::endTx(const uint8_t *silence) {
    while (m_txCount > 0) sendVoice(silence);
    uint8_t frame[YSF_FRAME_LENGTH_BYTES] = {};
    buildFich(frame, YSF_FI_TERMINATOR, 0);
    uint8_t csd1[CSD_BYTES], csd2[CSD_BYTES];
    headerFields(m_callsign, csd1, csd2);
    encodeHeaderData(csd1, csd2, frame);
    sendFrame(frame, true);
    std::fprintf(stderr, "ysf: PTT up\n");
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
