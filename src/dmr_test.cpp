// Standalone exercise of dmr_client's handshake against a real (or local
// test) Homebrew/MMDVM master -- mirrors roundtrip_test.cpp's role for the
// ThumbDV vocoder: verify the hardest new piece in isolation before wiring
// it into a frontend.
//
// Usage: dmr_test [options] <host> <port> <dmrId> <password> [callsign] [seconds] [txTalkgroup]
//   options (anywhere on the line):
//     --suffix NN         2-digit ESSID: logs in as repeater ID dmrId*100+NN
//                         (e.g. TGIF wants DMR ID + 2 digits)
//     --software-id S     RPTC software id (default "DV3000Client")
//     --package-id P      RPTC package id  (default "DV3000Client")
//   password "-" reads it from the DMR_PASSWORD environment variable
//   instead, so it never lands in shell history (and a "!" in it can't be
//   history-expanded).
//   txTalkgroup, if given, sends a short test transmission (a handful of
//   voice bursts with a dummy AMBE pattern -- not real audio, just enough
//   to exercise buildHeaderFrame/buildVoiceFrame/buildTerminatorFrame
//   against a real master) to that talkgroup right after linking.

#include "dmr_client.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {
void onSignal(int) { dmr::g_running = 0; }
} // namespace

int main(int rawArgc, char **rawArgv) {
    // Pull the --options out first, leaving the positional arguments.
    std::string suffix, softwareId = "DV3000Client", packageId = "DV3000Client";
    std::vector<char *> positional{rawArgv[0]};
    for (int i = 1; i < rawArgc; i++) {
        std::string a = rawArgv[i];
        if ((a == "--suffix" || a == "--software-id" || a == "--package-id") && i + 1 < rawArgc) {
            std::string &target = a == "--suffix" ? suffix : a == "--software-id" ? softwareId : packageId;
            target = rawArgv[++i];
        } else {
            positional.push_back(rawArgv[i]);
        }
    }
    int argc = static_cast<int>(positional.size());
    char **argv = positional.data();

    if (argc < 5) {
        std::fprintf(stderr,
                     "usage: %s [--suffix NN] [--software-id S] [--package-id P] <host> <port> <dmrId> <password|-> "
                     "[callsign] [seconds] [txTalkgroup]\n",
                     argv[0]);
        return 1;
    }

    std::string host = argv[1];
    uint16_t port = static_cast<uint16_t>(std::atoi(argv[2]));
    uint32_t dmrId = static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 10));
    std::string password = argv[4];
    if (password == "-") {
        const char *env = std::getenv("DMR_PASSWORD");
        if (!env || !*env) {
            std::fprintf(stderr, "dmr_test: password \"-\" needs the DMR_PASSWORD environment variable set\n");
            return 1;
        }
        password = env;
    }
    std::string callsign = argc >= 6 ? argv[5] : "TEST";
    int runSeconds = argc >= 7 ? std::atoi(argv[6]) : 15;
    bool doTx = argc >= 8;
    uint32_t txTalkgroup = doTx ? static_cast<uint32_t>(std::strtoul(argv[7], nullptr, 10)) : 0;

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    dmr::DmrClient client;
    if (!client.open(host, port)) return 1;

    dmr::RepeaterConfig config;
    config.callsign = callsign;
    config.description = "DV3000Client DMR test";
    config.softwareId = softwareId;
    config.packageId = packageId;
    uint32_t repeaterId = suffix.size() == 2 ? dmrId * 100 + static_cast<uint32_t>(std::strtoul(suffix.c_str(), nullptr, 10)) : 0;
    std::fprintf(stderr, "dmr_test: logging in as repeater ID %u (DMR ID %u), software id \"%s\", package id \"%s\"\n",
                 repeaterId ? repeaterId : dmrId, dmrId, softwareId.c_str(), packageId.c_str());
    client.setIdentity(dmrId, password, config, repeaterId);

    dmr::LinkResult result = client.link();
    std::fprintf(stderr, "dmr_test: link result: %s\n", dmr::ToString(result));
    if (result != dmr::LinkResult::Success) return 1;

    client.setVoiceRxSink([](const uint8_t *, const uint8_t *, const uint8_t *) {
        std::fprintf(stderr, "dmr_test: RX voice frame\n");
    });

    std::thread runner([&client] { client.run(); });

    if (doTx) {
        uint8_t ambe0[dmr::AMBE_FRAME_SIZE], ambe1[dmr::AMBE_FRAME_SIZE], ambe2[dmr::AMBE_FRAME_SIZE];
        for (int i = 0; i < dmr::AMBE_FRAME_SIZE; i++) {
            ambe0[i] = static_cast<uint8_t>(0x10 + i);
            ambe1[i] = static_cast<uint8_t>(0x20 + i);
            ambe2[i] = static_cast<uint8_t>(0x30 + i);
        }

        std::fprintf(stderr, "dmr_test: sending test transmission to TG%u\n", txTalkgroup);
        uint32_t streamId = client.beginVoiceTx(txTalkgroup);
        for (int burst = 0; burst < 10 && dmr::g_running; burst++) {
            client.sendVoiceFrame(streamId, ambe0, ambe1, ambe2);
            std::this_thread::sleep_for(std::chrono::milliseconds(60)); // one DMR voice burst = 60ms
        }
        client.endVoiceTx(streamId);
    }

    std::fprintf(stderr, "dmr_test: linked, running for %ds (Ctrl+C to stop early)\n", runSeconds);
    for (int i = 0; i < runSeconds && dmr::g_running; i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    dmr::g_running = 0;
    runner.join();

    client.disconnect();
    return 0;
}
