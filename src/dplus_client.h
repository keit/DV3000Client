#pragma once

// DPlus: the D-Star protocol REF reflectors and DPlus gateways (repeater
// gateways running Robin AA4RC's dplus, e.g. ZL2VH's "DPLUS 2.2t") speak to
// DV Dongles, hotspots and PC clients. Unlike DExtra, a client must first
// log in to the DPlus authentication server (authenticate()) -- that's how
// reflectors and gateways know the callsign is a registered D-Star user and
// where it's connecting from. The voice packets are DExtra's DSVT header,
// frame and last-frame packets with a 2-byte length/flags prefix.
//
// What a client is sent, found against REF001 and ZL2VH with dplus_test:
// - the streams of *every* module (xlxd's CDplusProtocol::HandleQueue: "all
//   stream of all modules are push to all clients") -- so a client can
//   monitor all of them, or pick one, without transmitting first;
// - from a gateway (ZL2VH), its CQ traffic (UR CQCQCQ), from RF users and
//   internet users alike; a transmission routed somewhere (UR "/ZL2VH C",
//   or a station's callsign) isn't sent to DPlus users at all.
// One login per callsign: a second gets BUSY. "CALL   X" logins with a
// suffix letter (how hotspots log in) count as separate users, so this can
// run alongside the operator's own hotspot or radio; a suffix that isn't a
// registered terminal gets a read-only login (OKRO): listen, not transmit.
//
// Packet formats and timings follow xlxd's server side and G4KLX's
// ircDDBGateway (DPlusAuthenticator, ConnectData, DPlusHandler).

#include <chrono>
#include <csignal>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "dstar_slowdata.h"
#include "dstar_transport.h"
#include "vocoder_pipeline.h"

namespace dplus {

constexpr int DPLUS_PORT = 20001;

// One entry of the authentication server's list: an active REF reflector
// or DPlus gateway.
struct GatewayEntry {
    std::string name;    // e.g. "REF001", "ZL2VH"
    std::string address; // IPv4 address
};

// Logs callsign in to the DPlus authentication server (TCP,
// auth.dstargateway.org:20001), which has to happen before a reflector or
// gateway will accept it. On success, fills gateways (if given) with the
// list the server sends back. ircDDBGateway repeats this periodically.
bool authenticate(const std::string &callsign, std::vector<GatewayEntry> *gateways, std::string &error);

// The module a stream is on: RPT2's ("REF001 C", or "ZL2VH  B" for traffic
// a gateway sends towards module B) -- unless RPT2 is the gateway itself
// ("ZL2VH  G", a radio keying the repeater), when it's RPT1's ("ZL2VH  C").
// RPT1 on a reflector is the sender's own hotspot ("KI4EYK D"), so it
// can't be the one looked at first. ' ' if neither says.
char streamModule(const dextra::DStarHeader &header);

enum class LinkResult {
    Accepted, // OKRW
    ReadOnly, // OKRO: may listen, not transmit (unregistered suffix)
    Refused,  // BUSY (this login is already connected) or anything else
    NoAnswer,
};

class DPlusClient : public dstar::DStarTransport {
public:
    ~DPlusClient() override;

    // callsign: the plain callsign; suffix: ' ' or the letter of one of its
    // registered terminals ('P' logs in as "ZL2MIM P", the way a hotspot
    // does), and transmits as MY "ZL2MIM P". myCall2: MYCALL2 of what we
    // transmit.
    void setIdentity(const std::string &callsign, char suffix, const std::string &myCall2 = "");
    // The reflector or gateway linked to (e.g. "REF001", "ZL2VH") and the
    // module to play and transmit on. '*' plays whichever module's stream
    // comes first, and transmitting isn't possible.
    void setTarget(const std::string &name, char module);
    // Live mode: the chosen stream's audio is decoded through vocoder
    // (DVRate3600x2400) and handed to sink, on vocoder's reply thread.
    void setLiveRx(audio::VocoderPipeline *vocoder, std::function<void(const short *)> sink) {
        m_vocoder = vocoder;
        m_liveRxSink = std::move(sink);
    }

    bool open(const std::string &host);
    // Connect request, then login; retries for a few seconds. The server's
    // 4-letter answer ("BUSY", ...) is left in loginReply().
    LinkResult link();
    const std::string &loginReply() const { return m_loginReply; }

    // dstar::DStarTransport. The header sink sees every stream's header,
    // played or not -- see the top of this file for why some never bring
    // audio.
    void setHeaderSink(std::function<void(const dextra::DStarHeader &)> sink) override { m_headerSink = std::move(sink); }
    uint16_t beginLiveTx() override;
    void sendLiveTxFrame(uint16_t streamId, const uint8_t *ambe) override;
    void endLiveTx(uint16_t streamId) override;
    void run() override; // until dextra::g_running is cleared
    void disconnect() override;

    // For dplus_test: every stream's header (with its ID) and voice frame,
    // raw -- last is the end-of-stream frame, which carries no audio.
    void setRawSinks(std::function<void(uint16_t, const dextra::DStarHeader &)> header,
                     std::function<void(uint16_t, uint8_t, const uint8_t *, bool)> frame) {
        m_rawHeaderSink = std::move(header);
        m_rawFrameSink = std::move(frame);
    }
    // Receive/keepalive loop; returns when *running is cleared, or after
    // 30 s without anything from the server.
    void run(const volatile std::sig_atomic_t *running);

    // Transmits 0.2 s of silence on the target module -- joins a gateway
    // module the way any transmission there would (dplus_test's
    // "kerchunk").
    void sendKerchunk();

private:
    ssize_t recvWithTimeout(uint8_t *buf, size_t len, int timeoutMs);
    void handlePacket(const uint8_t *buf, size_t len);
    void onHeader(uint16_t streamId, const dextra::DStarHeader &header);
    void onFrame(uint16_t streamId, uint8_t packetId, const uint8_t *ambe, bool last);
    // The log's "RX stream ... ended" line; how = why it's over.
    void endStream(uint16_t streamId, const char *how);
    void expireStreams();
    void sendHeader(uint16_t streamId);
    void sendFrame(uint16_t streamId, uint8_t packetId, const uint8_t *ambe);
    void sendLast(uint16_t streamId, uint8_t packetId);

    int m_fd = -1;
    std::string m_callsign = "N0CALL";
    std::string m_loginCallsign = "N0CALL";
    std::string m_myCall2;
    std::string m_target;
    char m_module = '*';
    std::string m_loginReply;

    audio::VocoderPipeline *m_vocoder = nullptr;
    std::function<void(const short *)> m_liveRxSink;
    std::function<void(const dextra::DStarHeader &)> m_headerSink;
    std::function<void(uint16_t, const dextra::DStarHeader &)> m_rawHeaderSink;
    std::function<void(uint16_t, uint8_t, const uint8_t *, bool)> m_rawFrameSink;

    // Receive (run()'s thread): streams whose header was already passed on
    // (headers are repeated), and the one being played, if any.
    std::vector<uint16_t> m_recentHeaders;
    int m_playing = -1;
    std::chrono::steady_clock::time_point m_playingLast;
    // Every stream heard, for the log: one line when it starts, one when
    // it ends -- including those a gateway sends without audio.
    struct RxStream {
        std::string who;
        char module;
        int frames = 0;
        std::chrono::steady_clock::time_point start, last;
        dextra::DStarHeader header;
        dstar::SlowDataDecoder slowData;
    };
    std::map<uint16_t, RxStream> m_rxStreams;

    // Transmit: the capture thread sends the header and the end, the
    // vocoder's reply thread the frames in between (the capture thread
    // drains the pipeline before ending, so they don't overlap).
    uint8_t m_txPacketId = 0;
};

} // namespace dplus
