// DExtra UDP protocol implementation and the ThumbDV-backed vocoder session.
//
// Protocol layout below is transcribed directly from xlxd's own source
// (cdextraprotocol.cpp / cdvheaderpacket.h / ccallsign.cpp) -- the actual
// server this talks to -- rather than reconstructed from memory.

#include "dextra_client.h"

#include <cmath>
#include <cstring>
#include <thread>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace dextra {

volatile sig_atomic_t g_running = 1;
std::mutex g_dvMutex;

namespace {

constexpr int DEXTRA_PORT = 30001;
constexpr int KEEPALIVE_PERIOD_SEC = 3;
constexpr int CALLSIGN_LEN = 8;
constexpr int DVDATA_SIZE = 3;

// "DSVT" + type/flag bytes, common to both header and frame packets.
const uint8_t HEADER_TAG[12] = {0x44, 0x53, 0x56, 0x54, 0x10, 0x00, 0x00, 0x00,
                                 0x20, 0x00, 0x01, 0x02};
const uint8_t FRAME_TAG[12]  = {0x44, 0x53, 0x56, 0x54, 0x20, 0x00, 0x00, 0x00,
                                 0x20, 0x00, 0x01, 0x02};
// Fixed idle AMBE+DVDATA pattern used on the final frame of a transmission
// (the last frame is always this marker, never real audio).
const uint8_t LASTFRAME_IDLE[AMBE_SIZE + DVDATA_SIZE] = {
    0x55, 0xC8, 0x7A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x25, 0x1A, 0xC6};

// 8-byte space-padded callsign with a trailing module letter in the 8th
// byte, matching CCallsign::SetCallsign's expectation (first 7 bytes are
// the name, byte 7 is the module).
std::vector<uint8_t> paddedCallsign(const char *base, char module) {
    std::vector<uint8_t> out(CALLSIGN_LEN, ' ');
    size_t n = std::strlen(base);
    for (size_t i = 0; i < n && i < CALLSIGN_LEN - 1; i++) {
        out[i] = static_cast<uint8_t>(base[i]);
    }
    out[CALLSIGN_LEN - 1] = static_cast<uint8_t>(module);
    return out;
}

std::string trimmed(const uint8_t *data, int len) {
    std::string s(reinterpret_cast<const char *>(data), len);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

} // namespace

DextraClient::DextraClient(SerialDV::DVController *dv, FILE *rxPcmOut)
    : m_dv(dv), m_rxPcmOut(rxPcmOut) {}

DextraClient::DextraClient(SerialDV::DVController *dv, std::function<void(const short *)> liveRxSink, FILE *rxPcmOut)
    : m_dv(dv), m_rxPcmOut(rxPcmOut), m_liveMode(true), m_liveRxSink(std::move(liveRxSink)) {}

void DextraClient::setIdentity(const std::string &callsign, char module) {
    m_ourCallsign = callsign.substr(0, 7);
    m_ourModule = module;
}

void DextraClient::setHeaderSink(std::function<void(const DStarHeader &)> sink) {
    m_liveHeaderSink = std::move(sink);
}

bool DextraClient::open(const std::string &host, char targetModule) {
    m_targetModule = targetModule;

    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *res = nullptr;
    char portStr[8];
    std::snprintf(portStr, sizeof(portStr), "%d", DEXTRA_PORT);
    if (getaddrinfo(host.c_str(), portStr, &hints, &res) != 0 || !res) {
        std::fprintf(stderr, "dextra_client: cannot resolve %s\n", host.c_str());
        return false;
    }

    m_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (m_fd < 0) {
        std::fprintf(stderr, "dextra_client: socket() failed\n");
        freeaddrinfo(res);
        return false;
    }

    if (::connect(m_fd, res->ai_addr, res->ai_addrlen) < 0) {
        std::fprintf(stderr, "dextra_client: connect() failed\n");
        freeaddrinfo(res);
        return false;
    }
    freeaddrinfo(res);

    std::fprintf(stderr, "dextra_client: opened UDP socket to %s:%d\n", host.c_str(), DEXTRA_PORT);
    return true;
}

bool DextraClient::link() {
    // 8-byte name field, then a dedicated my_module byte (authoritative --
    // IsValidConnectPacket calls SetModule(data[8]) explicitly, separate
    // from and overriding whatever SetCallsign's own parsing of the name
    // field would infer), then target module, then revision.
    std::vector<uint8_t> connectPkt = paddedCallsign(m_ourCallsign.c_str(), m_ourModule);
    connectPkt.push_back(static_cast<uint8_t>(m_ourModule));
    connectPkt.push_back(static_cast<uint8_t>(m_targetModule));
    connectPkt.push_back(0x00); // protocol revision 0 (plain client, not XRF/rev1)

    for (int attempt = 0; attempt < 5 && g_running; attempt++) {
        std::fprintf(stderr, "dextra_client: sending CONNECT for module %c (attempt %d)\n",
                     m_targetModule, attempt + 1);
        ::send(m_fd, connectPkt.data(), connectPkt.size(), 0);

        uint8_t buf[64];
        ssize_t n = recvWithTimeout(buf, sizeof(buf), 1000);
        if (n == 14 && std::memcmp(buf, connectPkt.data(), 10) == 0) {
            if (std::memcmp(buf + 10, "ACK", 3) == 0) {
                std::fprintf(stderr, "dextra_client: linked to module %c\n", m_targetModule);
                m_linked = true;
                m_lastKeepaliveSent = std::chrono::steady_clock::now();
                return true;
            }
            if (std::memcmp(buf + 10, "NAK", 3) == 0) {
                std::fprintf(stderr, "dextra_client: reflector NAKed the connect request\n");
                return false;
            }
        }
    }
    std::fprintf(stderr, "dextra_client: timed out waiting for connect ACK\n");
    return false;
}

void DextraClient::disconnect() {
    if (!m_linked) return;
    std::vector<uint8_t> pkt = paddedCallsign(m_ourCallsign.c_str(), m_ourModule);
    pkt.push_back(static_cast<uint8_t>(m_ourModule));
    pkt.push_back(' '); // space here (not a module letter) marks this as disconnect
    pkt.push_back(0x00);
    ::send(m_fd, pkt.data(), pkt.size(), 0);
    std::fprintf(stderr, "dextra_client: sent DISCONNECT\n");
    m_linked = false;
}

uint16_t DextraClient::beginLiveTx() {
    uint16_t streamId = nextStreamId();
    sendOriginatedHeader(streamId);
    std::fprintf(stderr, "dextra_client: PTT down, streamId=%u\n", streamId);
    return streamId;
}

void DextraClient::sendLiveTxFrame(uint16_t streamId, const uint8_t *ambe) {
    sendOriginatedFrame(streamId, m_liveTxPacketId, ambe, false);
    m_liveTxPacketId = static_cast<uint8_t>((m_liveTxPacketId + 1) % 21);
}

void DextraClient::endLiveTx(uint16_t streamId) {
    sendOriginatedFrame(streamId, m_liveTxPacketId, nullptr, true);
    m_liveTxPacketId = 0;
    std::fprintf(stderr, "dextra_client: PTT up, streamId=%u\n", streamId);
}

void DextraClient::sendTestTransmission(const std::string &pcmPath) {
    FILE *in = std::fopen(pcmPath.c_str(), "rb");
    if (!in) {
        std::fprintf(stderr, "dextra_client: cannot open %s\n", pcmPath.c_str());
        return;
    }

    std::vector<std::array<uint8_t, AMBE_SIZE>> frames;
    short pcm[SerialDV::MBE_AUDIO_BLOCK_SIZE];
    while (std::fread(pcm, sizeof(short), SerialDV::MBE_AUDIO_BLOCK_SIZE, in)
           == SerialDV::MBE_AUDIO_BLOCK_SIZE) {
        std::array<uint8_t, AMBE_SIZE> ambe{};
        bool ok;
        {
            std::lock_guard<std::mutex> lock(g_dvMutex);
            ok = m_dv->encode(pcm, ambe.data(), SerialDV::DVRate3600x2400);
        }
        if (!ok) {
            std::fprintf(stderr, "dextra_client: AMBE encode failed at frame %zu\n", frames.size());
            break;
        }
        frames.push_back(ambe);
    }
    std::fclose(in);

    uint16_t streamId = nextStreamId();
    std::fprintf(stderr, "dextra_client: sending test transmission from %s, streamId=%u, %zu frames\n",
                 pcmPath.c_str(), streamId, frames.size());
    sendOriginatedHeader(streamId);
    for (size_t i = 0; i < frames.size(); i++) {
        sendOriginatedFrame(streamId, static_cast<uint8_t>(i % 21), frames[i].data(), false);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    sendOriginatedFrame(streamId, static_cast<uint8_t>(frames.size() % 21), nullptr, true);
}

void DextraClient::run() {
    while (g_running) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - m_lastKeepaliveSent).count()
            >= KEEPALIVE_PERIOD_SEC) {
            sendKeepalive();
            m_lastKeepaliveSent = now;
        }

        uint8_t buf[64];
        ssize_t n = recvWithTimeout(buf, sizeof(buf), 200);
        if (n <= 0) continue;

        if (n == 56 && std::memcmp(buf, HEADER_TAG, 12) == 0) {
            onHeaderPacket(buf);
        } else if (n == 27 && std::memcmp(buf, FRAME_TAG, 12) == 0) {
            onFramePacket(buf);
        } else if (n == 9) {
            // the reflector's own keepalive to us -- nothing to do
        } else {
            std::fprintf(stderr, "dextra_client: unrecognized packet (%zd bytes)\n", n);
        }
    }
}

ssize_t DextraClient::recvWithTimeout(uint8_t *buf, size_t len, int timeoutMs) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(m_fd, &fds);
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int r = ::select(m_fd + 1, &fds, nullptr, nullptr, &tv);
    if (r <= 0) return 0;
    return ::recv(m_fd, buf, len, 0);
}

void DextraClient::sendKeepalive() {
    // 8-byte name field (module already embedded at byte 7, per
    // SetCallsign's own extraction -- IsValidKeepAlivePacket doesn't do a
    // separate SetModule call the way connect does) plus one more byte to
    // make size 9, matching xlxd's own EncodeKeepAlivePacket.
    std::vector<uint8_t> pkt = paddedCallsign(m_ourCallsign.c_str(), m_ourModule);
    pkt.push_back(static_cast<uint8_t>(m_ourModule));
    ::send(m_fd, pkt.data(), pkt.size(), 0);
}

void DextraClient::onHeaderPacket(const uint8_t *buf) {
    // dstar_header starts at offset 15: Flag1,2,3(3) RPT2[8] RPT1[8] UR[8] MY[8] SUFFIX[4] Crc[2]
    uint16_t streamId = static_cast<uint16_t>(buf[12]) | (static_cast<uint16_t>(buf[13]) << 8);
    // xlxd sends the header packet 5x for UDP reliability (see its
    // HandleQueue: n = IsDvHeader() ? 5 : 1) -- ignore repeats of a stream
    // we've already seen, whether still in progress or since completed,
    // rather than treating each copy as a new stream.
    if (streamId == m_lastRxStreamId) return;
    m_lastRxStreamId = streamId;

    DStarHeader header;
    header.rpt2 = trimmed(buf + 15 + 3, 8);
    header.rpt1 = trimmed(buf + 15 + 3 + 8, 8);
    header.urCall = trimmed(buf + 15 + 3 + 8 + 8, 8);
    header.myCall = trimmed(buf + 15 + 3 + 8 + 8 + 8, 8);
    header.myCall2 = trimmed(buf + 15 + 3 + 8 + 8 + 8 + 8, 4);

    std::fprintf(stderr, "dextra_client: RX header, streamId=%u, from %s/%s via %s,%s\n",
                 streamId, header.myCall.c_str(), header.myCall2.c_str(),
                 header.rpt1.c_str(), header.rpt2.c_str());

    m_rxStreamId = streamId;
    m_rxActive = true;
    m_rxAmbeFrames.clear();

    if (m_liveHeaderSink) m_liveHeaderSink(header);
}

void DextraClient::onFramePacket(const uint8_t *buf) {
    uint16_t streamId = static_cast<uint16_t>(buf[12]) | (static_cast<uint16_t>(buf[13]) << 8);
    if (!m_rxActive || streamId != m_rxStreamId) return;

    uint8_t packetId = buf[14];
    bool isLast = (packetId & 0x40) != 0;

    if (m_liveMode) {
        // Decode-on-arrival, straight to the playback sink -- no
        // buffering, and no echo (a real speaker replaces that stand-in).
        if (!isLast) {
            short pcm[SerialDV::MBE_AUDIO_BLOCK_SIZE];
            bool ok;
            {
                std::lock_guard<std::mutex> lock(g_dvMutex);
                ok = m_dv->decode(pcm, buf + 15, SerialDV::DVRate3600x2400);
            }
            if (ok) {
                if (m_liveRxSink) m_liveRxSink(pcm);
                if (m_rxPcmOut) std::fwrite(pcm, sizeof(short), SerialDV::MBE_AUDIO_BLOCK_SIZE, m_rxPcmOut);
            } else {
                std::fprintf(stderr, "dextra_client: AMBE decode failed, dropping frame\n");
            }
        } else {
            std::fprintf(stderr, "dextra_client: RX stream %u complete\n", m_rxStreamId);
            m_rxActive = false;
            if (m_rxPcmOut) std::fflush(m_rxPcmOut);
        }
        return;
    }

    if (!isLast) {
        std::array<uint8_t, AMBE_SIZE> ambe{};
        std::memcpy(ambe.data(), buf + 15, AMBE_SIZE);
        m_rxAmbeFrames.push_back(ambe);
    } else {
        std::fprintf(stderr, "dextra_client: RX stream %u complete, %zu frames -- echoing back\n",
                     m_rxStreamId, m_rxAmbeFrames.size());
        m_rxActive = false;
        echoTransmission();
    }
}

// Decodes each received AMBE frame to PCM through the ThumbDV, optionally
// saves that PCM for inspection, then re-encodes it back to AMBE and sends
// *that* -- rather than replaying the raw received AMBE bytes verbatim --
// so this actually exercises the vocoder round-trip, not just the network
// relay.
void DextraClient::echoTransmission() {
    uint16_t streamId = nextStreamId();
    sendOriginatedHeader(streamId);

    double sumSq = 0.0;
    for (size_t i = 0; i < m_rxAmbeFrames.size(); i++) {
        short pcm[SerialDV::MBE_AUDIO_BLOCK_SIZE];
        std::array<uint8_t, AMBE_SIZE> ambeOut{};
        const uint8_t *toSend = m_rxAmbeFrames[i].data();

        bool decodeOk, encodeOk = false;
        {
            std::lock_guard<std::mutex> lock(g_dvMutex);
            decodeOk = m_dv->decode(pcm, m_rxAmbeFrames[i].data(), SerialDV::DVRate3600x2400);
            if (decodeOk) encodeOk = m_dv->encode(pcm, ambeOut.data(), SerialDV::DVRate3600x2400);
        }

        if (!decodeOk) {
            std::fprintf(stderr, "dextra_client: AMBE decode failed at frame %zu, echoing raw\n", i);
        } else {
            if (m_rxPcmOut) std::fwrite(pcm, sizeof(short), SerialDV::MBE_AUDIO_BLOCK_SIZE, m_rxPcmOut);
            for (short s : pcm) sumSq += double(s) * s;

            if (!encodeOk) {
                std::fprintf(stderr, "dextra_client: AMBE encode failed at frame %zu, echoing raw\n", i);
            } else {
                toSend = ambeOut.data();
            }
        }

        sendOriginatedFrame(streamId, static_cast<uint8_t>(i % 21), toSend, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    sendOriginatedFrame(streamId, static_cast<uint8_t>(m_rxAmbeFrames.size() % 21), nullptr, true);
    if (m_rxPcmOut) std::fflush(m_rxPcmOut);

    unsigned int totalSamples = static_cast<unsigned int>(m_rxAmbeFrames.size()) * SerialDV::MBE_AUDIO_BLOCK_SIZE;
    std::fprintf(stderr, "dextra_client: echoed streamId=%u (%zu frames, decoded rms=%.1f)\n",
                 streamId, m_rxAmbeFrames.size(), totalSamples ? std::sqrt(sumSq / totalSamples) : 0.0);
}

uint16_t DextraClient::nextStreamId() {
    m_txStreamCounter++;
    if (m_txStreamCounter == 0) m_txStreamCounter = 1; // must stay non-zero
    return m_txStreamCounter;
}

void DextraClient::sendOriginatedHeader(uint16_t streamId) {
    std::vector<uint8_t> pkt(HEADER_TAG, HEADER_TAG + 12);
    pkt.push_back(static_cast<uint8_t>(streamId & 0xFF));
    pkt.push_back(static_cast<uint8_t>((streamId >> 8) & 0xFF));
    pkt.push_back(0x80);

    pkt.push_back(0x00); // Flag1
    pkt.push_back(0x00); // Flag2
    pkt.push_back(0x00); // Flag3

    // RPT2: module char (byte 7) is what xlxd actually routes on.
    auto rpt2 = paddedCallsign("XLXLOCAL", m_targetModule);
    pkt.insert(pkt.end(), rpt2.begin(), rpt2.end());

    auto rpt1 = paddedCallsign("XLXLOCAL", 'G');
    pkt.insert(pkt.end(), rpt1.begin(), rpt1.end());

    auto ur = paddedCallsign("CQCQCQ", ' ');
    pkt.insert(pkt.end(), ur.begin(), ur.end());

    auto my = paddedCallsign(m_ourCallsign.c_str(), m_ourModule);
    pkt.insert(pkt.end(), my.begin(), my.end());

    pkt.insert(pkt.end(), {' ', ' ', ' ', ' '}); // suffix, unused
    pkt.push_back(0x00); // Crc lo -- unchecked by xlxd
    pkt.push_back(0x00); // Crc hi

    // Sent twice for UDP reliability, matching the retry pattern used
    // elsewhere in reference DExtra client implementations.
    ::send(m_fd, pkt.data(), pkt.size(), 0);
    ::send(m_fd, pkt.data(), pkt.size(), 0);
}

void DextraClient::sendOriginatedFrame(uint16_t streamId, uint8_t packetId, const uint8_t *ambe, bool last) {
    std::vector<uint8_t> pkt(FRAME_TAG, FRAME_TAG + 12);
    pkt.push_back(static_cast<uint8_t>(streamId & 0xFF));
    pkt.push_back(static_cast<uint8_t>((streamId >> 8) & 0xFF));
    pkt.push_back(last ? (packetId | 0x40) : packetId);

    if (last) {
        pkt.insert(pkt.end(), LASTFRAME_IDLE, LASTFRAME_IDLE + AMBE_SIZE + DVDATA_SIZE);
    } else {
        pkt.insert(pkt.end(), ambe, ambe + AMBE_SIZE);
        pkt.insert(pkt.end(), {0x00, 0x00, 0x00}); // DVDATA (slow data), unused
    }

    ::send(m_fd, pkt.data(), pkt.size(), 0);
}

} // namespace dextra
