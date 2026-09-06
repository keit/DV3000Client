// Step 4 of the ThumbDV/XLX client build order: wire up a real microphone
// and speaker. Adds a "live" mode that plays back received audio to a
// speaker in real time (decode-on-arrival, no buffering) and transmits
// mic audio, encoded through the ThumbDV, while Space is held -- on top
// of (not replacing) the existing file-based echo/test modes, which stay
// as the regression-test path for automated/offline verification.
//
// Protocol layout below is transcribed directly from xlxd's own source
// (cdextraprotocol.cpp / cdvheaderpacket.h / ccallsign.cpp) -- the actual
// server this talks to -- rather than reconstructed from memory.
//
// Usage: dextra_test <host> <target module letter> <tty device> [test <input.raw> | <rx_output.raw> | live [capture-dev [playback-dev]]]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <csignal>
#include <cmath>
#include <array>
#include <vector>
#include <deque>
#include <string>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <functional>

#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <termios.h>

#include <alsa/asoundlib.h>

#include "dvcontroller.h"

namespace {

// The ThumbDV is one serial device running a synchronous request/response
// protocol (DVController::encode/decode) -- it can only serve one call at
// a time. Live mode has two threads that need it (capture thread encoding
// TX audio, network thread decoding RX audio for live playback), so every
// call to it anywhere in this file goes through this lock.
std::mutex g_dvMutex;

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

// Thin wrapper around one ALSA capture or playback stream, opened at
// D-Star's 8kHz/16-bit-mono rate with a period matching one DV frame (160
// samples = 20ms -- SerialDV::MBE_AUDIO_BLOCK_SIZE). read()/write() block
// for that period, which is what paces the capture/playback threads in
// live mode (replacing the manual sleep_for(20ms) the file-based paths use).
class AlsaPcm {
public:
    bool open(const std::string &device, snd_pcm_stream_t stream) {
        m_stream = stream;
        int err = snd_pcm_open(&m_handle, device.c_str(), stream, 0);
        if (err < 0) {
            std::fprintf(stderr, "dextra_test: snd_pcm_open(%s) failed: %s\n",
                         device.c_str(), snd_strerror(err));
            return false;
        }

        unsigned int rate = 8000;
        err = snd_pcm_set_params(m_handle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                                  1, rate, 1, 4 * (1000000 / (rate / SerialDV::MBE_AUDIO_BLOCK_SIZE)));
        if (err < 0) {
            std::fprintf(stderr, "dextra_test: snd_pcm_set_params(%s) failed: %s\n",
                         device.c_str(), snd_strerror(err));
            snd_pcm_close(m_handle);
            m_handle = nullptr;
            return false;
        }
        return true;
    }

    void close() {
        if (m_handle) {
            snd_pcm_close(m_handle);
            m_handle = nullptr;
        }
    }

    // Reads exactly SerialDV::MBE_AUDIO_BLOCK_SIZE samples, recovering from
    // over/underruns rather than treating them as fatal.
    bool read(short *pcm) {
        snd_pcm_sframes_t n = snd_pcm_readi(m_handle, pcm, SerialDV::MBE_AUDIO_BLOCK_SIZE);
        if (n == static_cast<snd_pcm_sframes_t>(SerialDV::MBE_AUDIO_BLOCK_SIZE)) return true;
        if (n < 0) return recover(static_cast<int>(n));
        return false;
    }

    bool write(const short *pcm) {
        snd_pcm_sframes_t n = snd_pcm_writei(m_handle, pcm, SerialDV::MBE_AUDIO_BLOCK_SIZE);
        if (n == static_cast<snd_pcm_sframes_t>(SerialDV::MBE_AUDIO_BLOCK_SIZE)) return true;
        if (n < 0) return recover(static_cast<int>(n));
        return false;
    }

private:
    bool recover(int err) {
        std::fprintf(stderr, "dextra_test: ALSA %s xrun/error: %s\n",
                     m_stream == SND_PCM_STREAM_CAPTURE ? "capture" : "playback", snd_strerror(err));
        return snd_pcm_recover(m_handle, err, 1) == 0;
    }

    snd_pcm_t *m_handle = nullptr;
    snd_pcm_stream_t m_stream = SND_PCM_STREAM_CAPTURE;
};

// Bounded queue of decoded PCM chunks handed from the network thread
// (live-mode RX decode) to the playback thread.
class PcmQueue {
public:
    void push(const short *pcm) {
        std::array<short, SerialDV::MBE_AUDIO_BLOCK_SIZE> chunk;
        std::memcpy(chunk.data(), pcm, sizeof(chunk));
        std::lock_guard<std::mutex> lock(m_mutex);
        // Drop the oldest chunk rather than growing unbounded if playback
        // ever falls behind -- a little audio loss beats unbounded latency.
        if (m_queue.size() > 50) m_queue.pop_front();
        m_queue.push_back(chunk);
        m_cv.notify_one();
    }

    // Waits up to timeoutMs for a chunk; returns false on timeout.
    bool pop(short *pcm, int timeoutMs) {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return !m_queue.empty(); })) {
            return false;
        }
        std::memcpy(pcm, m_queue.front().data(), sizeof(short) * SerialDV::MBE_AUDIO_BLOCK_SIZE);
        m_queue.pop_front();
        return true;
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::array<short, SerialDV::MBE_AUDIO_BLOCK_SIZE>> m_queue;
};

// Push-to-talk via a held key. A plain TTY has no physical key-up event --
// holding a key just produces repeated bytes from the OS's keyboard
// auto-repeat -- so "held" is approximated the standard way: PTT is
// considered active from the first Space byte until PTT_HANGTIME_MS passes
// with no further Space byte, functionally the same as a VOX hangtime but
// gated on the key rather than mic level.
constexpr int PTT_HANGTIME_MS = 200;

class PttInput {
public:
    bool start() {
        if (tcgetattr(STDIN_FILENO, &m_savedTermios) != 0) return false;
        struct termios raw = m_savedTermios;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 1; // 100ms read timeout, in tenths of a second
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return false;
        m_termiosSaved = true;

        std::fprintf(stderr, "dextra_test: hold SPACE to transmit, Ctrl+C to quit\n");
        m_thread = std::thread(&PttInput::run, this);
        return true;
    }

    void stop() {
        if (m_thread.joinable()) m_thread.join();
        if (m_termiosSaved) tcsetattr(STDIN_FILENO, TCSANOW, &m_savedTermios);
    }

    bool active() const { return m_active.load(); }

private:
    void run() {
        auto lastSpace = std::chrono::steady_clock::now() - std::chrono::hours(1);
        while (g_running) {
            char c;
            ssize_t n = ::read(STDIN_FILENO, &c, 1);
            auto now = std::chrono::steady_clock::now();
            if (n == 1 && c == ' ') {
                lastSpace = now;
                m_active.store(true);
            }
            if (m_active.load() &&
                std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSpace).count() > PTT_HANGTIME_MS) {
                m_active.store(false);
            }
        }
        m_active.store(false);
    }

    std::atomic<bool> m_active{false};
    std::thread m_thread;
    struct termios m_savedTermios{};
    bool m_termiosSaved = false;
};

class DextraClient {
public:
    DextraClient(SerialDV::DVController *dv, FILE *rxPcmOut)
        : m_dv(dv), m_rxPcmOut(rxPcmOut) {}

    // Live mode: instead of buffering a whole transmission and echoing it
    // back (the file-based modes' stand-in for a second endpoint), decode
    // each frame as it arrives and hand the PCM to liveRxSink for immediate
    // playback -- see onFramePacket(). rxPcmOut is optional here too, so a
    // live session can be recorded for offline quality inspection alongside
    // (not instead of) real-time playback.
    DextraClient(SerialDV::DVController *dv, std::function<void(const short *)> liveRxSink, FILE *rxPcmOut = nullptr)
        : m_dv(dv), m_rxPcmOut(rxPcmOut), m_liveMode(true), m_liveRxSink(std::move(liveRxSink)) {}

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

    // Live-mode TX, driven by the capture thread's PTT state machine. Only
    // that one thread calls these (RX in live mode never originates a
    // transmission -- see onFramePacket), so no locking is needed beyond
    // what UDP send() already gives for free; they just reuse the same
    // framing helpers the file-based paths use.
    uint16_t beginLiveTx() {
        uint16_t streamId = nextStreamId();
        sendOriginatedHeader(streamId);
        std::fprintf(stderr, "dextra_test: PTT down, streamId=%u\n", streamId);
        return streamId;
    }

    void sendLiveTxFrame(uint16_t streamId, const uint8_t *ambe) {
        sendOriginatedFrame(streamId, m_liveTxPacketId, ambe, false);
        m_liveTxPacketId = static_cast<uint8_t>((m_liveTxPacketId + 1) % 21);
    }

    void endLiveTx(uint16_t streamId) {
        sendOriginatedFrame(streamId, m_liveTxPacketId, nullptr, true);
        m_liveTxPacketId = 0;
        std::fprintf(stderr, "dextra_test: PTT up, streamId=%u\n", streamId);
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
            bool ok;
            {
                std::lock_guard<std::mutex> lock(g_dvMutex);
                ok = m_dv->encode(pcm, ambe.data(), SerialDV::DVRate3600x2400);
            }
            if (!ok) {
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
                    std::fprintf(stderr, "dextra_test: AMBE decode failed, dropping frame\n");
                }
            } else {
                std::fprintf(stderr, "dextra_test: RX stream %u complete\n", m_rxStreamId);
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

            bool decodeOk, encodeOk = false;
            {
                std::lock_guard<std::mutex> lock(g_dvMutex);
                decodeOk = m_dv->decode(pcm, m_rxAmbeFrames[i].data(), SerialDV::DVRate3600x2400);
                if (decodeOk) encodeOk = m_dv->encode(pcm, ambeOut.data(), SerialDV::DVRate3600x2400);
            }

            if (!decodeOk) {
                std::fprintf(stderr, "dextra_test: AMBE decode failed at frame %zu, echoing raw\n", i);
            } else {
                if (m_rxPcmOut) std::fwrite(pcm, sizeof(short), SerialDV::MBE_AUDIO_BLOCK_SIZE, m_rxPcmOut);
                for (short s : pcm) sumSq += double(s) * s;

                if (!encodeOk) {
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
    uint8_t m_liveTxPacketId = 0;

    SerialDV::DVController *m_dv;
    FILE *m_rxPcmOut;
    bool m_liveMode = false;
    std::function<void(const short *)> m_liveRxSink;
};

// Live-mode TX: PTT-driven mic -> ThumbDV -> network. Runs on its own
// thread; AlsaPcm::read() blocking for one ALSA period (20ms) is what
// paces it, the same role sleep_for(20ms) plays in the file-based paths.
void captureThread(SerialDV::DVController *dv, AlsaPcm *capture, DextraClient *client, const PttInput *ptt) {
    bool transmitting = false;
    uint16_t streamId = 0;
    short pcm[SerialDV::MBE_AUDIO_BLOCK_SIZE];

    while (g_running) {
        if (!capture->read(pcm)) continue;

        bool pttActive = ptt->active();
        if (pttActive && !transmitting) {
            streamId = client->beginLiveTx();
            transmitting = true;
        } else if (!pttActive && transmitting) {
            client->endLiveTx(streamId);
            transmitting = false;
        }
        if (!transmitting) continue;

        std::array<uint8_t, AMBE_SIZE> ambe{};
        bool ok;
        {
            std::lock_guard<std::mutex> lock(g_dvMutex);
            ok = dv->encode(pcm, ambe.data(), SerialDV::DVRate3600x2400);
        }
        if (ok) {
            client->sendLiveTxFrame(streamId, ambe.data());
        } else {
            std::fprintf(stderr, "dextra_test: AMBE encode failed, dropping frame\n");
        }
    }

    if (transmitting) client->endLiveTx(streamId);
}

// Live-mode RX playback: drains decoded PCM chunks pushed by the network
// thread's onFramePacket(). If nothing arrives for a while (no active RX
// stream), it just idles -- ALSA's own buffering absorbs the gap, no need
// to fill it with explicit silence.
void playbackThread(AlsaPcm *playback, PcmQueue *queue) {
    short pcm[SerialDV::MBE_AUDIO_BLOCK_SIZE];
    while (g_running) {
        if (queue->pop(pcm, 100)) {
            playback->write(pcm);
        }
    }
}

} // namespace

void usage(const char *prog) {
    std::fprintf(stderr, "usage: %s <host> <target module letter> <tty device> [test <input.raw> | <rx_output.raw> | live [capture-dev [playback-dev [rx_output.raw]]]]\n", prog);
    std::fprintf(stderr, "  no extra arg: link and echo received transmissions back through the ThumbDV\n");
    std::fprintf(stderr, "  <rx_output.raw>: also save decoded RX PCM (8kHz/16-bit-LE) there for inspection\n");
    std::fprintf(stderr, "  'test' <input.raw>: encode that PCM file via the ThumbDV and send it as one "
                          "transmission, then exit (needs a second instance running to observe the echo "
                          "-- a reflector never relays a stream back to its own originator)\n");
    std::fprintf(stderr, "  'live': hold SPACE to transmit captured mic audio, and play back received audio "
                          "in real time. capture-dev/playback-dev default to ALSA's \"default\" PCM; "
                          "run `arecord -l` / `aplay -l` to see actual device names on this machine. "
                          "rx_output.raw there too saves decoded RX PCM (8kHz/16-bit-LE) alongside live playback\n");
}

int main(int argc, char **argv) {
    if (argc < 4 || argc > 8) {
        usage(argv[0]);
        return 1;
    }

    bool sendTest = false;
    bool liveMode = false;
    std::string testPcmPath, rxOutPath, liveRxOutPath;
    std::string captureDev = "default", playbackDev = "default";

    if (argc == 4) {
        // file-based echo mode, no extra arg
    } else if (std::strcmp(argv[4], "test") == 0) {
        if (argc != 6) { usage(argv[0]); return 1; }
        sendTest = true;
        testPcmPath = argv[5];
    } else if (std::strcmp(argv[4], "live") == 0) {
        liveMode = true;
        if (argc >= 6) captureDev = argv[5];
        if (argc >= 7) playbackDev = argv[6];
        if (argc >= 8) liveRxOutPath = argv[7];
    } else if (argc == 5) {
        rxOutPath = argv[4];
    } else {
        usage(argv[0]);
        return 1;
    }

    SerialDV::DVController dv;
    if (!dv.open(argv[3])) {
        std::fprintf(stderr, "dextra_test: failed to open %s\n", argv[3]);
        return 1;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    if (liveMode) {
        AlsaPcm capture, playback;
        if (!capture.open(captureDev, SND_PCM_STREAM_CAPTURE)) return 1;
        if (!playback.open(playbackDev, SND_PCM_STREAM_PLAYBACK)) return 1;

        FILE *liveRxPcmOut = nullptr;
        if (!liveRxOutPath.empty()) {
            liveRxPcmOut = std::fopen(liveRxOutPath.c_str(), "wb");
            if (!liveRxPcmOut) {
                std::fprintf(stderr, "dextra_test: cannot open %s for writing\n", liveRxOutPath.c_str());
                return 1;
            }
        }

        PcmQueue rxQueue;
        DextraClient client(&dv, [&rxQueue](const short *pcm) { rxQueue.push(pcm); }, liveRxPcmOut);
        if (!client.open(argv[1], argv[2][0])) return 1;
        if (!client.link()) return 1;

        PttInput ptt;
        if (!ptt.start()) {
            std::fprintf(stderr, "dextra_test: failed to set up PTT input (not a terminal?)\n");
            return 1;
        }

        std::thread capThread(captureThread, &dv, &capture, &client, &ptt);
        std::thread playThread(playbackThread, &playback, &rxQueue);

        client.run(); // blocks until g_running is cleared (SIGINT/SIGTERM)

        capThread.join();
        playThread.join();
        ptt.stop();
        capture.close();
        playback.close();

        client.disconnect();
        if (liveRxPcmOut) std::fclose(liveRxPcmOut);
        dv.close();
        return 0;
    }

    FILE *rxPcmOut = nullptr;
    if (!rxOutPath.empty()) {
        rxPcmOut = std::fopen(rxOutPath.c_str(), "wb");
        if (!rxPcmOut) {
            std::fprintf(stderr, "dextra_test: cannot open %s for writing\n", rxOutPath.c_str());
            return 1;
        }
    }

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
