// Standalone exercise of dmr_client's handshake against a real (or local
// test) Homebrew/MMDVM master -- mirrors roundtrip_test.cpp's role for the
// ThumbDV vocoder: verify the hardest new piece in isolation before wiring
// it into a frontend.
//
// Usage: dmr_test <host> <port> <dmrId> <password> [callsign] [seconds to run]

#include "dmr_client.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {
void onSignal(int) { dmr::g_running = 0; }
} // namespace

int main(int argc, char **argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: %s <host> <port> <dmrId> <password> [callsign] [seconds]\n", argv[0]);
        return 1;
    }

    std::string host = argv[1];
    uint16_t port = static_cast<uint16_t>(std::atoi(argv[2]));
    uint32_t dmrId = static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 10));
    std::string password = argv[4];
    std::string callsign = argc >= 6 ? argv[5] : "TEST";
    int runSeconds = argc >= 7 ? std::atoi(argv[6]) : 15;

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    dmr::DmrClient client;
    if (!client.open(host, port)) return 1;

    dmr::RepeaterConfig config;
    config.callsign = callsign;
    config.description = "DV3000Client DMR test";
    client.setIdentity(dmrId, password, config);

    dmr::LinkResult result = client.link();
    std::fprintf(stderr, "dmr_test: link result: %s\n", dmr::ToString(result));
    if (result != dmr::LinkResult::Success) return 1;

    std::thread runner([&client] { client.run(); });

    std::fprintf(stderr, "dmr_test: linked, running for %ds (Ctrl+C to stop early)\n", runSeconds);
    for (int i = 0; i < runSeconds && dmr::g_running; i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    dmr::g_running = 0;
    runner.join();

    client.disconnect();
    return 0;
}
