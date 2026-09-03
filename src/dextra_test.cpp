// Step 3 of the ThumbDV/XLX client build order: route AMBE frames received
// over DExtra through a real ThumbDV (decode to PCM, then re-encode before
// echoing), and source the test transmission's AMBE from a real PCM file
// via the ThumbDV rather than a synthetic byte pattern -- proving the full
// network+vocoder path end to end before wiring up an actual mic/speaker.
//
// Protocol layout below is transcribed directly from xlxd's own source
// (cdextraprotocol.cpp / cdvheaderpacket.h / ccallsign.cpp) -- the actual
// server this talks to -- rather than reconstructed from memory.
//
// Usage: dextra_test <host> <target module letter> <tty device> [test <input.raw> | <rx_output.raw>]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <csignal>
#include <cmath>
#include <array>
#include <vector>
#include <string>
#include <chrono>
#include <thread>

#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "dvcontroller.h"

namespace {

constexpr int DEXTRA_PORT = 30001;
constexpr int KEEPALIVE_PERIOD_SEC = 3;
constexpr int CALLSIGN_LEN = 8;
constexpr int AMBE_SIZE = 9;
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

const char *OUR_CALLSIGN = "ZL2MIM";
const char OUR_MODULE = 'B';

volatile sig_atomic_t g_running = 1;

void onSignal(int) { g_running = 0; }

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

class DextraClient {
public:
    DextraClient(SerialDV::DVController *dv, FILE *rxPcmOut)
        : m_dv(dv), m_rxPcmOut(rxPcmOut) {}

    bool open(const std::string &host, char targetModule) {
        m_targetModule = targetModule;

        struct addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        struct addrinfo *res = nullptr;
        char portStr[8];
        std::snprintf(portStr, sizeof(portStr), "%d", DEXTRA_PORT);
        if (getaddrinfo(host.c_str(), portStr, &hints, &res) != 0 || !res) {
            std::fprintf(stderr, "dextra_test: cannot resolve %s\n", host.c_str());
            return false;
        }

        m_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (m_fd < 0) {
            std::fprintf(stderr, "dextra_test: socket() failed\n");
            freeaddrinfo(res);
            return false;
        }

        if (::connect(m_fd, res->ai_addr, res->ai_addrlen) < 0) {
            std::fprintf(stderr, "dextra_test: connect() failed\n");
            freeaddrinfo(res);
            return false;
        }
        freeaddrinfo(res);

        std::fprintf(stderr, "dextra_test: opened UDP socket to %s:%d\n", host.c_str(), DEXTRA_PORT);
        return true;
    }

    // Sends CONNECT and waits (with retries) for a 14-byte ACK/NAK.
    bool link() {
        // 8-byte name field, then a dedicated my_module byte (authoritative --
        // IsValidConnectPacket calls SetModule(data[8]) explicitly, separate
        // from and overriding whatever SetCallsign's own parsing of the name
        // field would infer), then target module, then revision.
        std::vector<uint8_t> connectPkt = paddedCallsign(OUR_CALLSIGN, OUR_MODULE);
        connectPkt.push_back(static_cast<uint8_t>(OUR_MODULE));
        connectPkt.push_back(static_cast<uint8_t>(m_targetModule));
        connectPkt.push_back(0x00); // protocol revision 0 (plain client, not XRF/rev1)

        for (int attempt = 0; attempt < 5 && g_running; attempt++) {
            std::fprintf(stderr, "dextra_test: sending CONNECT for module %c (attempt %d)\n",
                         m_targetModule, attempt + 1);
            ::send(m_fd, connectPkt.data(), connectPkt.size(), 0);

            uint8_t buf[64];
            ssize_t n = recvWithTimeout(buf, sizeof(buf), 1000);
            if (n == 14 && std::memcmp(buf, connectPkt.data(), 10) == 0) {
                if (std::memcmp(buf + 10, "ACK", 3) == 0) {
                    std::fprintf(stderr, "dextra_test: linked to module %c\n", m_targetModule);
                    m_linked = true;
                    m_lastKeepaliveSent = std::chrono::steady_clock::now();
                    return true;
                }
                if (std::memcmp(buf + 10, "NAK", 3) == 0) {
                    std::fprintf(stderr, "dextra_test: reflector NAKed the connect request\n");
                    return false;
                }
            }
        }
        std::fprintf(stderr, "dextra_test: timed out waiting for connect ACK\n");
        return false;
    }

    void disconnect() {
        if (!m_linked) return;
        std::vector<uint8_t> pkt = paddedCallsign(OUR_CALLSIGN, OUR_MODULE);
        pkt.push_back(static_cast<uint8_t>(OUR_MODULE));
        pkt.push_back(' '); // space here (not a module letter) marks this as disconnect
        pkt.push_back(0x00);
        ::send(m_fd, pkt.data(), pkt.size(), 0);
        std::fprintf(stderr, "dextra_test: sent DISCONNECT\n");
        m_linked = false;
    }

    // Originates one transmission encoding real PCM audio (via the ThumbDV,
    // same rate/format as roundtrip_test) rather than a synthetic AMBE byte
    // pattern -- for exercising/verifying the transmit path against a real
    // reflector, since a reflector never relays a stream back to its own
    // originator, so this needs a *second* client instance running to
    // actually observe the echo).
    void sendTestTransmission(const std::string &pcmPath) {
        FILE *in = std::fopen(pcmPath.c_str(), "rb");
        if (!in) {
            std::fprintf(stderr, "dextra_test: cannot open %s\n", pcmPath.c_str());
            return;
        }

        std::vector<std::array<uint8_t, AMBE_SIZE>> frames;
        short pcm[SerialDV::MBE_AUDIO_BLOCK_SIZE];
        while (std::fread(pcm, sizeof(short), SerialDV::MBE_AUDIO_BLOCK_SIZE, in)
               == SerialDV::MBE_AUDIO_BLOCK_SIZE) {
            std::array<uint8_t, AMBE_SIZE> ambe{};
            if (!m_dv->encode(pcm, ambe.data(), SerialDV::DVRate3600x2400)) {
                std::fprintf(stderr, "dextra_test: AMBE encode failed at frame %zu\n", frames.size());
                break;
            }
            frames.push_back(ambe);
        }
        std::fclose(in);

        uint16_t streamId = nextStreamId();
        std::fprintf(stderr, "dextra_test: sending test transmission from %s, streamId=%u, %zu frames\n",
                     pcmPath.c_str(), streamId, frames.size());
        sendOriginatedHeader(streamId);
        for (size_t i = 0; i < frames.size(); i++) {
            sendOriginatedFrame(streamId, static_cast<uint8_t>(i % 21), frames[i].data(), false);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        sendOriginatedFrame(streamId, static_cast<uint8_t>(frames.size() % 21), nullptr, true);
    }

    // Main receive/keepalive/echo loop. Returns when g_running is cleared.
    void run() {
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
                std::fprintf(stderr, "dextra_test: unrecognized packet (%zd bytes)\n", n);
            }
        }
    }

private:
    ssize_t recvWithTimeout(uint8_t *buf, size_t len, int timeoutMs) {
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

    void sendKeepalive() {
        // 8-byte name field (module already embedded at byte 7, per
        // SetCallsign's own extraction -- IsValidKeepAlivePacket doesn't
        // do a separate SetModule call the way connect does) plus one
        // more byte to make size 9, matching xlxd's own EncodeKeepAlivePacket.
        std::vector<uint8_t> pkt = paddedCallsign(OUR_CALLSIGN, OUR_MODULE);
        pkt.push_back(static_cast<uint8_t>(OUR_MODULE));
        ::send(m_fd, pkt.data(), pkt.size(), 0);
    }

    void onHeaderPacket(const uint8_t *buf) {
        // dstar_header starts at offset 15: Flag1,2,3(3) RPT2[8] RPT1[8] UR[8] MY[8] SUFFIX[4] Crc[2]
        uint16_t streamId = static_cast<uint16_t>(buf[12]) | (static_cast<uint16_t>(buf[13]) << 8);
        // xlxd sends the header packet 5x for UDP reliability (see its
        // HandleQueue: n = IsDvHeader() ? 5 : 1) -- ignore repeats of a
        // stream we've already seen, whether still in progress or since
        // completed, rather than treating each copy as a new stream.
        if (streamId == m_lastRxStreamId) return;
        m_lastRxStreamId = streamId;

        std::string myCall = trimmed(buf + 15 + 3 + 8 + 8 + 8, 8); // MY field

        std::fprintf(stderr, "dextra_test: RX header, streamId=%u, from %s\n", streamId, myCall.c_str());

        m_rxStreamId = streamId;
        m_rxActive = true;
        m_rxAmbeFrames.clear();
    }

    void onFramePacket(const uint8_t *buf) {
        uint16_t streamId = static_cast<uint16_t>(buf[12]) | (static_cast<uint16_t>(buf[13]) << 8);
        if (!m_rxActive || streamId != m_rxStreamId) return;

        uint8_t packetId = buf[14];
        bool isLast = (packetId & 0x40) != 0;

        if (!isLast) {
            std::array<uint8_t, AMBE_SIZE> ambe{};
            std::memcpy(ambe.data(), buf + 15, AMBE_SIZE);
            m_rxAmbeFrames.push_back(ambe);
        } else {
            std::fprintf(stderr, "dextra_test: RX stream %u complete, %zu frames -- echoing back\n",
                         m_rxStreamId, m_rxAmbeFrames.size());
            m_rxActive = false;
            echoTransmission();
        }
    }

    // Decodes each received AMBE frame to PCM through the ThumbDV, optionally
    // saves that PCM for inspection, then re-encodes it back to AMBE and
    // sends *that* -- rather than replaying the raw received AMBE bytes
    // verbatim -- so this actually exercises the vocoder round-trip, not
    // just the network relay.
    void echoTransmission() {
        uint16_t streamId = nextStreamId();
        sendOriginatedHeader(streamId);

        double sumSq = 0.0;
        for (size_t i = 0; i < m_rxAmbeFrames.size(); i++) {
            short pcm[SerialDV::MBE_AUDIO_BLOCK_SIZE];
            std::array<uint8_t, AMBE_SIZE> ambeOut{};
            const uint8_t *toSend = m_rxAmbeFrames[i].data();

            if (!m_dv->decode(pcm, m_rxAmbeFrames[i].data(), SerialDV::DVRate3600x2400)) {
                std::fprintf(stderr, "dextra_test: AMBE decode failed at frame %zu, echoing raw\n", i);
            } else {
                if (m_rxPcmOut) std::fwrite(pcm, sizeof(short), SerialDV::MBE_AUDIO_BLOCK_SIZE, m_rxPcmOut);
                for (short s : pcm) sumSq += double(s) * s;

                if (!m_dv->encode(pcm, ambeOut.data(), SerialDV::DVRate3600x2400)) {
                    std::fprintf(stderr, "dextra_test: AMBE encode failed at frame %zu, echoing raw\n", i);
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
        std::fprintf(stderr, "dextra_test: echoed streamId=%u (%zu frames, decoded rms=%.1f)\n",
                     streamId, m_rxAmbeFrames.size(), totalSamples ? std::sqrt(sumSq / totalSamples) : 0.0);
    }

    uint16_t nextStreamId() {
        m_txStreamCounter++;
        if (m_txStreamCounter == 0) m_txStreamCounter = 1; // must stay non-zero
        return m_txStreamCounter;
    }

    void sendOriginatedHeader(uint16_t streamId) {
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

        auto my = paddedCallsign(OUR_CALLSIGN, OUR_MODULE);
        pkt.insert(pkt.end(), my.begin(), my.end());

        pkt.insert(pkt.end(), {' ', ' ', ' ', ' '}); // suffix, unused
        pkt.push_back(0x00); // Crc lo -- unchecked by xlxd
        pkt.push_back(0x00); // Crc hi

        // Sent twice for UDP reliability, matching the retry pattern used
        // elsewhere in reference DExtra client implementations.
        ::send(m_fd, pkt.data(), pkt.size(), 0);
        ::send(m_fd, pkt.data(), pkt.size(), 0);
    }

    void sendOriginatedFrame(uint16_t streamId, uint8_t packetId, const uint8_t *ambe, bool last) {
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

    int m_fd = -1;
    char m_targetModule = 'A';
    bool m_linked = false;
    std::chrono::steady_clock::time_point m_lastKeepaliveSent;

    bool m_rxActive = false;
    uint16_t m_rxStreamId = 0;
    uint16_t m_lastRxStreamId = 0xFFFF; // sentinel: no valid stream uses this on the wire in practice here
    std::vector<std::array<uint8_t, AMBE_SIZE>> m_rxAmbeFrames;

    uint16_t m_txStreamCounter = 0;

    SerialDV::DVController *m_dv;
    FILE *m_rxPcmOut;
};

} // namespace

void usage(const char *prog) {
    std::fprintf(stderr, "usage: %s <host> <target module letter> <tty device> [test <input.raw> | <rx_output.raw>]\n", prog);
    std::fprintf(stderr, "  no extra arg: link and echo received transmissions back through the ThumbDV\n");
    std::fprintf(stderr, "  <rx_output.raw>: also save decoded RX PCM (8kHz/16-bit-LE) there for inspection\n");
    std::fprintf(stderr, "  'test' <input.raw>: encode that PCM file via the ThumbDV and send it as one "
                          "transmission, then exit (needs a second instance running to observe the echo "
                          "-- a reflector never relays a stream back to its own originator)\n");
}

int main(int argc, char **argv) {
    if (argc < 4 || argc > 6) {
        usage(argv[0]);
        return 1;
    }

    bool sendTest = false;
    std::string testPcmPath, rxOutPath;
    if (argc == 6 && std::strcmp(argv[4], "test") == 0) {
        sendTest = true;
        testPcmPath = argv[5];
    } else if (argc == 5) {
        rxOutPath = argv[4];
    } else if (argc != 4) {
        usage(argv[0]);
        return 1;
    }

    SerialDV::DVController dv;
    if (!dv.open(argv[3])) {
        std::fprintf(stderr, "dextra_test: failed to open %s\n", argv[3]);
        return 1;
    }

    FILE *rxPcmOut = nullptr;
    if (!rxOutPath.empty()) {
        rxPcmOut = std::fopen(rxOutPath.c_str(), "wb");
        if (!rxPcmOut) {
            std::fprintf(stderr, "dextra_test: cannot open %s for writing\n", rxOutPath.c_str());
            return 1;
        }
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    DextraClient client(&dv, rxPcmOut);
    if (!client.open(argv[1], argv[2][0])) return 1;
    if (!client.link()) return 1;

    if (sendTest) {
        // One-shot sender: fire the test transmission and exit. Do NOT also
        // run() here -- this instance would then receive and echo its own
        // transmission's echo back, forever, if another instance echoes it
        // (two unconditional "parrot" clients on the same module will always
        // ping-pong; that's real behavior, not a bug, but a one-shot sender
        // should not become one side of that pair by accident).
        client.sendTestTransmission(testPcmPath);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    } else {
        client.run();
    }

    client.disconnect();
    if (rxPcmOut) std::fclose(rxPcmOut);
    dv.close();
    return 0;
}
