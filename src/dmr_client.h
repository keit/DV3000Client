#pragma once

// DMR Homebrew/MMDVM repeater protocol client -- the protocol BrandMeister,
// DMR+, and other Homebrew-based DMR networks use for hotspot/repeater
// connections. A separate, independent protocol from DExtra/D-Star (see
// dextra_client.h) -- different transport framing, ID-based rather than
// callsign-based addressing, and a real login/auth handshake -- so this is
// a standalone client, not an extension of DextraClient.
//
// This first milestone covers the handshake and keepalive/receive loop
// (RPTL -> RPTK -> RPTC, then RPTPING/MSTPONG) -- enough to confirm a real
// master (or a local test xlxd instance, which speaks the same wire
// protocol server-side) accepts the connection. DMRD voice framing (which
// needs BPTC/Golay/QR/Hamming FEC encoding on top of the raw AMBE bytes,
// unlike D-Star's much simpler raw-AMBE-in-a-packet framing) is a later
// step once this is confirmed working end to end.
//
// Wire format confirmed against two independent, cross-checked sources:
// the vendored third_party/xlxd's server-side implementation
// (cdmrmmdvmprotocol.cpp) and g4klx/DMRGateway's client-side implementation
// (the software real BrandMeister hotspots run today).

#include <cstdint>
#include <csignal>
#include <string>

namespace dmr {

// Cleared by SIGINT/SIGTERM (installed by the frontend) to unwind run()'s
// loop cooperatively -- same pattern as dextra::g_running, but independent
// of it: this client has no dependency on dextra_client.h.
extern volatile sig_atomic_t g_running;

constexpr uint16_t DEFAULT_PORT = 62030;
constexpr int KEEPALIVE_PERIOD_SEC = 10; // matches DMRMMDVM_KEEPALIVE_PERIOD

enum class LinkResult {
    Success,
    LoginRejected,  // MSTNAK after RPTL -- DMR ID not recognised/permitted
    AuthRejected,   // MSTNAK after RPTK -- wrong password
    ConfigRejected, // MSTNAK after RPTC -- master rejected the declared config
    Timeout,
};

const char *ToString(LinkResult result);

// The RPTC packet's declared repeater info -- mostly cosmetic for a
// hotspot-style connection (shown on the network's dashboard), not
// re-validated by the master beyond basic sanity/size checks.
struct RepeaterConfig {
    std::string callsign;
    uint32_t rxFrequencyHz = 438800000;
    uint32_t txFrequencyHz = 438800000;
    unsigned power = 1;     // watts, 0-99
    unsigned colorCode = 1; // 0-15
    float latitude = 0.0f;
    float longitude = 0.0f;
    int heightMeters = 0;
    std::string location = "Unknown";
    std::string description = "DV3000Client";
    std::string url = "https://github.com/";
};

class DmrClient {
public:
    bool open(const std::string &host, uint16_t port = DEFAULT_PORT);

    // dmrId is this client's own (repeater/hotspot) DMR ID. Must be called
    // before link().
    void setIdentity(uint32_t dmrId, const std::string &password, const RepeaterConfig &config);

    // Runs the full RPTL -> RPTK -> RPTC handshake, retrying each step
    // (matching DExtra's link() retry pattern) until it succeeds, is
    // rejected, or times out.
    LinkResult link();

    // Sends RPTCL and closes the socket.
    void disconnect();

    // Keepalive/receive loop: sends RPTPING every KEEPALIVE_PERIOD_SEC and
    // logs whatever comes back. Returns when g_running is cleared.
    void run();

private:
    ssize_t recvWithTimeout(uint8_t *buf, size_t len, int timeoutMs);
    void sendPing();

    int m_fd = -1;
    uint32_t m_dmrId = 0;
    std::string m_password;
    RepeaterConfig m_config;
    uint8_t m_salt[4] = {};
};

} // namespace dmr
