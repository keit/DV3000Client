#pragma once

// Reusable DExtra/ThumbDV protocol client: the DExtra UDP wire protocol and
// the ThumbDV-backed vocoder session (DextraClient). No audio I/O and no
// terminal/CLI-specific code lives here -- audio capture/playback plumbing
// is in dextra_audio.h (see its header comment for why that's separate),
// and terminal PTT is in dextra_test.cpp. This is meant to be linked by any
// frontend, CLI or GUI, including ones that don't need live audio at all.

#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "dvcontroller.h"

namespace dextra {

constexpr int AMBE_SIZE = 9;

// Cleared by SIGINT/SIGTERM (installed by the frontend) to unwind every
// blocking loop in this library -- DextraClient::run()/link(), and
// dextra_audio's captureThread/playbackThread -- cooperatively.
extern volatile sig_atomic_t g_running;

// The ThumbDV is one serial device running a synchronous request/response
// protocol (DVController::encode/decode) -- it can only serve one call at
// a time. Live mode has two threads that need it (dextra_audio's capture
// thread encoding TX audio, DextraClient's own network thread decoding RX
// audio), so every call to it, from any file, goes through this lock.
extern std::mutex g_dvMutex;

class DextraClient {
public:
    DextraClient(SerialDV::DVController *dv, FILE *rxPcmOut);

    // Live mode: instead of buffering a whole transmission and echoing it
    // back, decode each frame as it arrives and hand the PCM to liveRxSink
    // for immediate playback -- see onFramePacket(). rxPcmOut is optional
    // here too, so a live session can be recorded for offline quality
    // inspection alongside (not instead of) real-time playback.
    DextraClient(SerialDV::DVController *dv, std::function<void(const short *)> liveRxSink, FILE *rxPcmOut = nullptr);

    // Our own callsign and module letter, sent in CONNECT/keepalive/echo
    // packets. Defaults match the previous hardcoded values; call before
    // link() to change them. Callsign is truncated to 7 characters (the
    // 8th byte of the field is always the module).
    void setIdentity(const std::string &callsign, char module);

    bool open(const std::string &host, char targetModule);

    // Sends CONNECT and waits (with retries) for a 14-byte ACK/NAK.
    bool link();
    void disconnect();

    // Live-mode TX, driven by the capture thread's PTT state machine. Only
    // that one thread calls these, so no locking is needed beyond what UDP
    // send() already gives for free.
    uint16_t beginLiveTx();
    void sendLiveTxFrame(uint16_t streamId, const uint8_t *ambe);
    void endLiveTx(uint16_t streamId);

    // Originates one transmission encoding real PCM audio via the ThumbDV,
    // for exercising/verifying the transmit path against a real reflector
    // (needs a *second* client instance running to observe the echo, since
    // a reflector never relays a stream back to its own originator).
    void sendTestTransmission(const std::string &pcmPath);

    // Main receive/keepalive/echo loop. Returns when g_running is cleared.
    void run();

private:
    ssize_t recvWithTimeout(uint8_t *buf, size_t len, int timeoutMs);
    void sendKeepalive();
    void onHeaderPacket(const uint8_t *buf);
    void onFramePacket(const uint8_t *buf);
    void echoTransmission();
    uint16_t nextStreamId();
    void sendOriginatedHeader(uint16_t streamId);
    void sendOriginatedFrame(uint16_t streamId, uint8_t packetId, const uint8_t *ambe, bool last);

    int m_fd = -1;
    std::string m_ourCallsign = "ZL2MIM";
    char m_ourModule = 'B';
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

} // namespace dextra
