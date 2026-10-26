#pragma once

// YSF (Yaesu System Fusion) network client: the UDP protocol YSF reflectors
// (G4KLX's YSFReflector, and xlxd's YSF side) speak to hotspots and
// YSFGateway.
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
//
// Transmitting sends DN mode the way Fusion radios do: a header frame
// carrying the callsigns, voice frames of 5 AMBE+2 frames each (frame
// numbers 0-6, their data channels cycling through destination, source,
// downlink, uplink and radio data), and a terminator. The data channels are
// coded by ysf_dch.

#include <atomic>
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
    int dgId = -1;           // the frames' DG-ID (00-99), -1 until a FICH decodes
};

class YsfClient {
public:
    ~YsfClient();

    bool open(const std::string &host, int port = DEFAULT_PORT);
    // DG-ID (Digital Group ID, 0-99) to transmit with -- the FICH's 7-bit
    // field older code calls SQ. Plain YSFReflectors ignore it; reflectors
    // with rooms (YCS, PA7LIM's) use it to pick one, e.g. JAPANLINK's main
    // room is 09; there, transmitting with a DG-ID moves you to that room.
    // May be changed while linked: the next transmission uses it.
    void setDgId(int dgId) { m_dgId = dgId; }
    // Text for a "YSFO" options packet (as YSFGateway's Options setting:
    // up to 36 characters), sent once linked and after every poll; e.g. a
    // static list of DG-IDs to book on a reflector that supports it.
    // Empty (the default): not sent.
    void setOptions(const std::string &options) { m_options = options; }
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

    // Transmit, as the linked callsign. sendVoice() takes one 72-bit AMBE+2
    // frame (DMR's format, as the ThumbDV encodes at DMR's rate) and sends a
    // YSF frame for every 5; endTx() pads a partial one with silence (a
    // frame the caller encoded from silent audio) and sends the terminator.
    // Frames go out as they're completed: the caller paces them.
    void beginTx();
    void sendVoice(const uint8_t *ambe);
    void endTx(const uint8_t *silence);

private:
    ssize_t recvWithTimeout(uint8_t *buf, size_t len, int timeoutMs);
    void sendPoll();
    void sendOptions();
    void handleData(const uint8_t *buf);
    void endStream(const char *how);
    void sendFrame(const uint8_t *frame, bool last);
    void buildFich(uint8_t *frame, unsigned char fi, unsigned char fn);

    int m_fd = -1;
    std::string m_callsign = "N0CALL";
    std::atomic<int> m_dgId{0};
    std::string m_options;

    std::function<void(const StreamInfo &)> m_startSink;
    std::function<void(const uint8_t *)> m_voiceSink;
    std::function<void(const StreamInfo &, int)> m_endSink;

    bool m_inStream = false;
    StreamInfo m_stream;
    int m_voiceFrames = 0;
    int m_frames = 0;
    std::chrono::steady_clock::time_point m_streamStart, m_lastData;

    // Transmit: the 5 AMBE frames of the YSF frame being filled, its frame
    // number (0-6), and the YSFD packet counter.
    uint8_t m_txAmbe[5][AMBE_FRAME_SIZE] = {};
    int m_txCount = 0;
    unsigned char m_txFn = 0;
    unsigned char m_txCounter = 0;
};

} // namespace ysf
