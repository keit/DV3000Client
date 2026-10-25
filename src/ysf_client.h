#pragma once

// YSF (Yaesu System Fusion) network client: the UDP protocol YSF reflectors
// (G4KLX's YSFReflector, and xlxd's YSF side) speak to hotspots and
// YSFGateway. Receive only so far.
//
// The protocol is small: "YSFP" + callsign polls every 5 s keep a client
// linked (the reflector answers each), "YSFU" + callsign unlinks, and
// voice travels as 155-byte "YSFD" packets -- gateway, source and
// destination callsigns (10 bytes each), a frame counter whose bit 0 marks
// the end of a transmission, then one 120-byte over-the-air YSF frame
// (100 ms): sync, FICH (frame type, data type, frame number) and payload.
//
// Of the data types, DN mode (V/D mode 2) is what most Fusion radios send
// by default: each frame carries 5 voice channels of AMBE+2 at the rate DMR
// uses, so each is converted (xlxd's CYsfUtils) to the 72-bit frame a
// ThumbDV decodes at DMR's rate (SerialDV::DVRate3600x2450). VW mode (full
// rate) and V/D mode 1 aren't decoded yet; their streams are logged.

#include <chrono>
#include <csignal>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ysf {

constexpr int DEFAULT_PORT = 42000;
constexpr size_t AMBE_FRAME_SIZE = 9; // DMR's 72-bit AMBE+2 frame

// One entry of the public YSF reflector list (Pi-Star's YSF_Hosts.txt).
struct ReflectorEntry {
    std::string id;   // 5-digit number, e.g. "21080"
    std::string name; // e.g. "US-America-Link"
    std::string description;
    std::string host;
    int port = DEFAULT_PORT;
};

// Fetches and parses the list (HTTP, a few hundred KB).
bool fetchReflectorList(std::vector<ReflectorEntry> &out, std::string &error);
std::vector<ReflectorEntry> parseReflectorList(const std::string &text);

// What a stream says about itself, from its YSFD packets and FICH.
struct StreamInfo {
    std::string source;      // the talking station, e.g. "JA1ABC"
    std::string destination; // usually "ALL"
    std::string gateway;     // the hotspot or repeater it came through
    int dataType = -1;       // YSF_DT_*: 2 = DN, 3 = VW, 0 = V/D mode 1, 1 = data
};

class YsfClient {
public:
    ~YsfClient();

    bool open(const std::string &host, int port = DEFAULT_PORT);
    // Polls until the reflector answers (a few seconds at most).
    bool link(const std::string &callsign);
    void disconnect();

    // Called on run()'s thread: when a stream starts, for each of its voice
    // frames (DN mode only), and when it ends.
    void setStreamStartSink(std::function<void(const StreamInfo &)> sink) { m_startSink = std::move(sink); }
    void setVoiceSink(std::function<void(const uint8_t *ambe)> sink) { m_voiceSink = std::move(sink); }
    void setStreamEndSink(std::function<void(const StreamInfo &, int voiceFrames)> sink) { m_endSink = std::move(sink); }

    // Receive/poll loop; returns when *running is cleared, or after 30 s
    // without anything from the reflector.
    void run(const volatile std::sig_atomic_t *running);

private:
    ssize_t recvWithTimeout(uint8_t *buf, size_t len, int timeoutMs);
    void sendPoll();
    void handleData(const uint8_t *buf);
    void endStream(const char *how);

    int m_fd = -1;
    std::string m_callsign = "N0CALL";

    std::function<void(const StreamInfo &)> m_startSink;
    std::function<void(const uint8_t *)> m_voiceSink;
    std::function<void(const StreamInfo &, int)> m_endSink;

    bool m_inStream = false;
    StreamInfo m_stream;
    int m_voiceFrames = 0;
    int m_frames = 0;
    std::chrono::steady_clock::time_point m_streamStart, m_lastData;
};

} // namespace ysf
