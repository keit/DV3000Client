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
#include "vocoder_pipeline.h"

namespace dextra {

constexpr int AMBE_SIZE = 9;

// The 5 human-readable callsign fields carried in every D-Star header
// (dstar_header, see onHeaderPacket()'s layout comment) -- who's
// transmitting and the repeater path the transmission took to get here.
struct DStarHeader {
    std::string rpt2;    // repeater 2 (the reflector + module actually routing this stream)
    std::string rpt1;    // repeater 1 (the gateway the originator is keying through)
    std::string urCall;  // called station, or "CQCQCQ" for a general call
    std::string myCall;  // originating station's callsign
    std::string myCall2; // originator's 4-char suffix (extension/module)
};

// Cleared by SIGINT/SIGTERM (installed by the frontend) to unwind every
// blocking loop in this library -- DextraClient::run()/link(),
// dextra_audio's captureThread, and the audio::playbackThread a D-Star
// session runs with this flag -- cooperatively.
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

    // Live mode: decode through vocoder instead of waiting on dv frame by
    // frame, so a slow ThumbDV link doesn't hold up the network thread (see
    // vocoder_pipeline.h). The sink then runs on vocoder's reply thread.
    // Set before link(); dv must then not be used for anything else.
    void setVocoder(audio::VocoderPipeline *vocoder) { m_vocoder = vocoder; }

    // Our own callsign and module letter, sent in CONNECT/keepalive/echo
    // packets. Defaults match the previous hardcoded values; call before
    // link() to change them. Callsign is truncated to 7 characters (the
    // 8th byte of the field is always the module). myCall2 is the
    // separate 4-byte MYCALL2 suffix field sent in an originated
    // transmission's header (sendOriginatedHeader) -- distinct from the
    // module byte above, which is part of the callsign field itself;
    // truncated to 4 characters, blank (space-padded) by default.
    void setIdentity(const std::string &callsign, char module, const std::string &myCall2 = "");

    // Live mode only: called with each newly-seen transmission's header
    // fields as soon as it arrives (see onHeaderPacket()), before any of
    // its audio frames. Optional -- a frontend that doesn't care who's
    // talking can leave this unset.
    void setHeaderSink(std::function<void(const DStarHeader &)> sink);

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
    std::string m_ourMyCall2;
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
    audio::VocoderPipeline *m_vocoder = nullptr;
    FILE *m_rxPcmOut;
    bool m_liveMode = false;
    std::function<void(const short *)> m_liveRxSink;
    std::function<void(const DStarHeader &)> m_liveHeaderSink;
};

} // namespace dextra
