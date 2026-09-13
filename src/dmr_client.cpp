#include "dmr_client.h"

#include "sha256.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace dmr {

volatile sig_atomic_t g_running = 1;

const char *ToString(LinkResult result) {
    switch (result) {
    case LinkResult::Success: return "success";
    case LinkResult::LoginRejected: return "login rejected (DMR ID not recognised/permitted)";
    case LinkResult::AuthRejected: return "authentication rejected (wrong password)";
    case LinkResult::ConfigRejected: return "configuration rejected";
    case LinkResult::Timeout: return "timed out";
    }
    return "unknown";
}

namespace {

// Big-endian, matching both cdmrmmdvmprotocol.cpp's MAKEDWORD/MAKEWORD
// parsing and DMRGateway's own m_id[0]=id>>24 construction.
void appendDmrId(std::vector<uint8_t> &pkt, uint32_t id) {
    pkt.push_back(static_cast<uint8_t>(id >> 24));
    pkt.push_back(static_cast<uint8_t>(id >> 16));
    pkt.push_back(static_cast<uint8_t>(id >> 8));
    pkt.push_back(static_cast<uint8_t>(id));
}

void appendTag(std::vector<uint8_t> &pkt, const char *tag) {
    pkt.insert(pkt.end(), tag, tag + std::strlen(tag));
}

// Fixed-width field: left-justified, space-padded/truncated to exactly
// `width` bytes -- matches the "%-N.Ns" pieces of DMRGateway's config
// sprintf.
void appendField(std::vector<uint8_t> &pkt, const std::string &s, size_t width) {
    for (size_t i = 0; i < width; i++) {
        pkt.push_back(i < s.size() ? static_cast<uint8_t>(s[i]) : static_cast<uint8_t>(' '));
    }
}

// Right-justified, zero-padded numeric field of exactly `width` decimal
// digits -- matches "%0Nu"/"%0Nd".
void appendNumericField(std::vector<uint8_t> &pkt, long value, size_t width) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%0*ld", static_cast<int>(width), value);
    pkt.insert(pkt.end(), buf, buf + width);
}

} // namespace

bool DmrClient::open(const std::string &host, uint16_t port) {
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *res = nullptr;
    char portStr[8];
    std::snprintf(portStr, sizeof(portStr), "%u", port);
    if (getaddrinfo(host.c_str(), portStr, &hints, &res) != 0 || !res) {
        std::fprintf(stderr, "dmr_client: cannot resolve %s\n", host.c_str());
        return false;
    }

    m_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (m_fd < 0) {
        std::fprintf(stderr, "dmr_client: socket() failed\n");
        freeaddrinfo(res);
        return false;
    }

    if (::connect(m_fd, res->ai_addr, res->ai_addrlen) < 0) {
        std::fprintf(stderr, "dmr_client: connect() failed\n");
        freeaddrinfo(res);
        return false;
    }
    freeaddrinfo(res);

    std::fprintf(stderr, "dmr_client: opened UDP socket to %s:%u\n", host.c_str(), port);
    return true;
}

void DmrClient::setIdentity(uint32_t dmrId, const std::string &password, const RepeaterConfig &config) {
    m_dmrId = dmrId;
    m_password = password;
    m_config = config;
}

ssize_t DmrClient::recvWithTimeout(uint8_t *buf, size_t len, int timeoutMs) {
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

LinkResult DmrClient::link() {
    // Step 1: RPTL -> RPTACK(+4-byte salt) or MSTNAK.
    std::vector<uint8_t> rptl;
    appendTag(rptl, "RPTL");
    appendDmrId(rptl, m_dmrId);

    bool gotAck = false;
    for (int attempt = 0; attempt < 5 && g_running && !gotAck; attempt++) {
        std::fprintf(stderr, "dmr_client: sending RPTL (attempt %d)\n", attempt + 1);
        ::send(m_fd, rptl.data(), rptl.size(), 0);

        uint8_t buf[64];
        ssize_t n = recvWithTimeout(buf, sizeof(buf), 1000);
        if (n == 10 && std::memcmp(buf, "RPTACK", 6) == 0) {
            std::memcpy(m_salt, buf + 6, 4);
            gotAck = true;
        } else if (n == 6 && std::memcmp(buf, "MSTNAK", 6) == 0) {
            std::fprintf(stderr, "dmr_client: RPTL rejected\n");
            return LinkResult::LoginRejected;
        }
    }
    if (!gotAck) {
        std::fprintf(stderr, "dmr_client: timed out waiting for RPTL ack\n");
        return LinkResult::Timeout;
    }
    std::fprintf(stderr, "dmr_client: RPTL acknowledged, salt=%02x%02x%02x%02x\n", m_salt[0], m_salt[1], m_salt[2],
                 m_salt[3]);

    // Step 2: RPTK -> RPTACK or MSTNAK.
    uint8_t digest[32];
    dmr::sha256(m_salt, m_password, digest);

    std::vector<uint8_t> rptk;
    appendTag(rptk, "RPTK");
    appendDmrId(rptk, m_dmrId);
    rptk.insert(rptk.end(), digest, digest + 32);

    gotAck = false;
    for (int attempt = 0; attempt < 5 && g_running && !gotAck; attempt++) {
        std::fprintf(stderr, "dmr_client: sending RPTK (attempt %d)\n", attempt + 1);
        ::send(m_fd, rptk.data(), rptk.size(), 0);

        uint8_t buf[64];
        ssize_t n = recvWithTimeout(buf, sizeof(buf), 1000);
        if (n == 6 && std::memcmp(buf, "RPTACK", 6) == 0) {
            gotAck = true;
        } else if (n == 6 && std::memcmp(buf, "MSTNAK", 6) == 0) {
            std::fprintf(stderr, "dmr_client: RPTK rejected -- wrong password?\n");
            return LinkResult::AuthRejected;
        }
    }
    if (!gotAck) {
        std::fprintf(stderr, "dmr_client: timed out waiting for RPTK ack\n");
        return LinkResult::Timeout;
    }
    std::fprintf(stderr, "dmr_client: RPTK (authentication) acknowledged\n");

    // Step 3: RPTC -> RPTACK or MSTNAK. Field widths/order match
    // DMRGateway.cpp's getConfig(): callsign(8) rxFreq(9) txFreq(9)
    // power(2) colorCode(2) latitude(8) longitude(9) height(3)
    // location(20) description(19) slots(1) url(124) swid(40) pkgid(40)
    // = 294 bytes, +4-byte tag +4-byte id = 302 total.
    std::vector<uint8_t> rptc;
    appendTag(rptc, "RPTC");
    appendDmrId(rptc, m_dmrId);
    appendField(rptc, m_config.callsign, 8);
    appendNumericField(rptc, m_config.rxFrequencyHz, 9);
    appendNumericField(rptc, m_config.txFrequencyHz, 9);
    appendNumericField(rptc, m_config.power > 99 ? 99 : m_config.power, 2);
    appendNumericField(rptc, m_config.colorCode > 15 ? 15 : m_config.colorCode, 2);
    {
        char lat[32];
        std::snprintf(lat, sizeof(lat), "%08f", static_cast<double>(m_config.latitude));
        appendField(rptc, lat, 8);
        char lon[32];
        std::snprintf(lon, sizeof(lon), "%09f", static_cast<double>(m_config.longitude));
        appendField(rptc, lon, 9);
    }
    appendNumericField(rptc, m_config.heightMeters, 3);
    appendField(rptc, m_config.location, 20);
    appendField(rptc, m_config.description, 19);
    rptc.push_back(static_cast<uint8_t>('4')); // slots: '4' = simplex/no duplex, matches a hotspot-style single client
    appendField(rptc, m_config.url, 124);
    appendField(rptc, "DV3000Client", 40); // software id
    appendField(rptc, "MMDVM", 40);        // package id -- some masters gate features on recognising this string

    gotAck = false;
    for (int attempt = 0; attempt < 5 && g_running && !gotAck; attempt++) {
        std::fprintf(stderr, "dmr_client: sending RPTC (attempt %d)\n", attempt + 1);
        ::send(m_fd, rptc.data(), rptc.size(), 0);

        uint8_t buf[64];
        ssize_t n = recvWithTimeout(buf, sizeof(buf), 1000);
        if (n == 6 && std::memcmp(buf, "RPTACK", 6) == 0) {
            gotAck = true;
        } else if (n == 6 && std::memcmp(buf, "MSTNAK", 6) == 0) {
            std::fprintf(stderr, "dmr_client: RPTC rejected\n");
            return LinkResult::ConfigRejected;
        }
    }
    if (!gotAck) {
        std::fprintf(stderr, "dmr_client: timed out waiting for RPTC ack\n");
        return LinkResult::Timeout;
    }
    std::fprintf(stderr, "dmr_client: RPTC (configuration) acknowledged -- linked\n");

    return LinkResult::Success;
}

void DmrClient::disconnect() {
    std::vector<uint8_t> pkt;
    appendTag(pkt, "RPTCL");
    appendDmrId(pkt, m_dmrId);
    ::send(m_fd, pkt.data(), pkt.size(), 0);
    std::fprintf(stderr, "dmr_client: sent RPTCL\n");
}

void DmrClient::sendPing() {
    std::vector<uint8_t> pkt;
    appendTag(pkt, "RPTPING");
    appendDmrId(pkt, m_dmrId);
    ::send(m_fd, pkt.data(), pkt.size(), 0);
}

void DmrClient::run() {
    auto lastPing = std::chrono::steady_clock::now();
    sendPing();

    while (g_running) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastPing).count() >= KEEPALIVE_PERIOD_SEC) {
            sendPing();
            lastPing = now;
        }

        uint8_t buf[128];
        ssize_t n = recvWithTimeout(buf, sizeof(buf), 200);
        if (n <= 0) continue;

        if (n == 11 && std::memcmp(buf, "MSTPONG", 7) == 0) {
            std::fprintf(stderr, "dmr_client: keepalive pong\n");
        } else if (n >= 5 && std::memcmp(buf, "MSTCL", 5) == 0) {
            std::fprintf(stderr, "dmr_client: master closed the connection\n");
            g_running = 0;
        } else if (n == 55 && std::memcmp(buf, "DMRD", 4) == 0) {
            std::fprintf(stderr, "dmr_client: DMRD voice frame received (%zd bytes) -- not yet decoded\n", n);
        } else {
            std::fprintf(stderr, "dmr_client: unrecognized packet (%zd bytes)\n", n);
        }
    }
}

} // namespace dmr
