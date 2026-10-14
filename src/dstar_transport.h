#pragma once

// What the D-Star tab and dextra::captureThread need from a D-Star network
// connection, whichever protocol it speaks -- DExtra (XLX/XRF reflectors,
// DextraClient) or DPlus (REF reflectors and DPlus repeater gateways,
// dplus::DPlusClient). Same idea as dmr::DmrTransport on the DMR side.

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace dextra {

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

} // namespace dextra

namespace dstar {

// For the log: what we put in a transmitted header, from its 5 callsign
// fields in wire order (RPT2 RPT1 UR MY MY2, 36 bytes) -- in the same
// layout as the received-stream lines, padding inside a field kept
// ("ZL2VH  C"), trailing padding dropped.
inline std::string describeTxHeader(const uint8_t *fields, const std::string &txMessage) {
    auto field = [fields](int offset, int n) {
        std::string s(reinterpret_cast<const char *>(fields + offset), static_cast<size_t>(n));
        size_t end = s.find_last_not_of(' ');
        return end == std::string::npos ? std::string() : s.substr(0, end + 1);
    };
    std::string my2 = field(32, 4);
    std::string s = field(24, 8) + (my2.empty() ? "" : "/" + my2) + " (UR " + field(16, 8) + ", RPT1 " +
                    field(8, 8) + ", RPT2 " + field(0, 8);
    if (!txMessage.empty()) s += ", TX message \"" + txMessage + "\"";
    return s + ")";
}

class DStarTransport {
public:
    virtual ~DStarTransport() = default;

    // Each received transmission's header, as it arrives (on run()'s thread).
    virtual void setHeaderSink(std::function<void(const dextra::DStarHeader &)> sink) = 0;

    // Live transmit, driven by the capture thread's PTT state machine.
    virtual uint16_t beginLiveTx() = 0;
    virtual void sendLiveTxFrame(uint16_t streamId, const uint8_t *ambe) = 0;
    virtual void endLiveTx(uint16_t streamId) = 0;

    // Receive/keepalive loop; returns when dextra::g_running is cleared (or
    // the connection is lost).
    virtual void run() = 0;
    virtual void disconnect() = 0;

    // The TX message sent in what we transmit (see dstar_slowdata.h).
    void setTxMessage(const std::string &message) { m_txMessage = message; }
    // A received transmission's TX message, once it's all in (on run()'s
    // thread), with that transmission's header to tell whose it is.
    void setTxMessageSink(std::function<void(const dextra::DStarHeader &, const std::string &)> sink) {
        m_txMessageSink = std::move(sink);
    }

protected:
    std::string m_txMessage;
    std::function<void(const dextra::DStarHeader &, const std::string &)> m_txMessageSink;
};

} // namespace dstar
