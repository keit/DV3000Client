#pragma once

// DPlus: the D-Star protocol REF reflectors and DPlus gateways (repeater
// gateways running Robin AA4RC's dplus, e.g. ZL2VH's "DPLUS 2.2t") speak to
// DV Dongles, hotspots and PC clients. Unlike DExtra, a client must first
// log in to the DPlus authentication server (authenticate()) -- that's how
// reflectors and gateways know the callsign is a registered D-Star user and
// where it's connecting from. The voice packets are DExtra's DSVT header,
// frame and last-frame packets with a 2-byte length/flags prefix.
//
// Receive side: a DPlus client is sent the streams of *every* module, and
// picks for itself which to play (see xlxd's CDplusProtocol::HandleQueue:
// "for dplus protocol, all stream of all modules are push to all clients").
// That's what lets a client monitor a repeater's traffic regardless of how
// it was routed. Packet formats and timings follow xlxd's server side and
// G4KLX's ircDDBGateway (DPlusAuthenticator, ConnectData, DPlusHandler).

#include <csignal>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "dextra_client.h" // DStarHeader -- the same D-Star header fields

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

class DPlusClient {
public:
    ~DPlusClient();

    bool open(const std::string &host);
    // Connect request, then login with callsign; true once the server
    // answers OKRW. Retries for a few seconds.
    bool link(const std::string &callsign);
    void disconnect();

    // Transmits a moment of silence (header, 10 silent frames, end) on
    // gateway module `module`: the header's first repeater field (RPT2 in
    // the D-Star spec's order) "<gateway> <module>", the second "<gateway>
    // G", UR CQCQCQ, MY callsign -- the way DExtra and xlxd's DPlus read a
    // client's module, and how the gateway itself sends a module's traffic.
    // (Tried the other way round, as a radio keying the repeater carries
    // them, DPLUS 2.2t logged it as a transmission on module "G".)
    // A DPlus gateway sends a stream's *audio* only to remote users on its
    // module, and a remote user joins a module by transmitting on it --
    // headers of other modules' streams arrive regardless.
    void sendKerchunk(const std::string &gateway, char module, const std::string &callsign);

    // Called on the run() thread for each new stream's header (repeats of
    // an already-seen header aren't passed on), and for each voice frame.
    // last is the end-of-stream frame, which carries no audio.
    void setHeaderSink(std::function<void(uint16_t streamId, const dextra::DStarHeader &)> sink) {
        m_headerSink = std::move(sink);
    }
    void setFrameSink(std::function<void(uint16_t streamId, uint8_t packetId, const uint8_t *ambe, bool last)> sink) {
        m_frameSink = std::move(sink);
    }

    // Receive/keepalive loop; returns when *running is cleared, or after
    // 30 s without anything from the server.
    void run(const volatile std::sig_atomic_t *running);

private:
    ssize_t recvWithTimeout(uint8_t *buf, size_t len, int timeoutMs);
    void handlePacket(const uint8_t *buf, size_t len);

    int m_fd = -1;
    std::vector<uint16_t> m_recentHeaders; // stream IDs whose header was already passed on
    std::function<void(uint16_t, const dextra::DStarHeader &)> m_headerSink;
    std::function<void(uint16_t, uint8_t, const uint8_t *, bool)> m_frameSink;
};

} // namespace dplus
