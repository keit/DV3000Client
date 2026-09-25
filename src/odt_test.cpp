// Standalone spike CLI for BrandMeister's Open DMR Terminal (Rewind)
// protocol -- connects, authenticates, subscribes to talkgroups, plays
// received voice back through a real ThumbDV + ALSA, and (optionally)
// transmits mic audio to one of them. See dmr_rewind.h's header comment
// for what's confirmed vs. still unverified.
//
// Now that RewindClient implements DmrTransport, both directions reuse
// dmr_audio.h's existing capture/playback threads as-is -- the same
// functions DmrTab uses for Homebrew.
//
// Usage: odt_test <host> <dmrId> <password> <thumbdv-device> <playback-device> [<capture-device> <tx-talkgroup> <group|private>] [rx-talkgroup ...]
//   dmrId must be your real DMR ID -- it's the terminal DMR ID field of
//   the initial KeepAlive, and confirmed against a live master that
//   BrandMeister ties it to the account the password belongs to; an
//   unrelated placeholder value gets a correctly-authenticated session
//   silently dropped.
//   password is BrandMeister's "Hotspot Security" password from
//   SelfCare, the same one an existing Homebrew/MMDVM connection
//   already uses.
//   If capture-device, tx-talkgroup and a call type are given, PTT is
//   driven by stdin: press Enter to toggle transmit on/off. Any
//   rx-talkgroup arguments after that are additional receive-only
//   subscriptions (tx-talkgroup is always subscribed too). For a Parrot
//   round-trip test (TG 9990), the call type MUST be "private" --
//   Parrot doesn't respond to a Group call, confirmed live (an earlier
//   version of this tool hardcoded Group and got no echo back at all).

#include "dextra_audio.h" // AlsaPcm, PcmQueue
#include "dmr_audio.h"    // captureThread, makeVoiceRxHandler, playbackThread
#include "dmr_rewind.h"
#include "dmr_transport.h" // dmr::g_running
#include "dvcontroller.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {
void handleSignal(int) { dmr::g_running = 0; }
} // namespace

int main(int argc, char **argv) {
    if (argc < 6 || argc == 7 || argc == 8) {
        std::fprintf(stderr,
                      "usage: %s <host> <dmrId> <password> <thumbdv-device> <playback-device> "
                      "[<capture-device> <tx-talkgroup> <group|private>] [rx-talkgroup ...]\n",
                      argv[0]);
        return 1;
    }
    std::string host = argv[1];
    uint32_t dmrId = static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 10));
    std::string password = argv[3];
    std::string thumbdvDevice = argv[4];
    std::string playbackDevice = argv[5];

    bool doTx = argc >= 9;
    std::string captureDevice = doTx ? argv[6] : "";
    uint32_t txTalkgroup = doTx ? static_cast<uint32_t>(std::strtoul(argv[7], nullptr, 10)) : 0;
    bool txPrivate = doTx && std::string(argv[8]) == "private";
    if (doTx && !txPrivate && std::string(argv[8]) != "group") {
        std::fprintf(stderr, "odt_test: call type must be exactly \"group\" or \"private\", got \"%s\"\n", argv[8]);
        return 1;
    }
    int rxArgStart = doTx ? 9 : 6;

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    SerialDV::DVController dv;
    if (!dv.open(thumbdvDevice)) {
        std::fprintf(stderr, "odt_test: failed to open ThumbDV device %s\n", thumbdvDevice.c_str());
        return 1;
    }

    dextra::AlsaPcm playback;
    if (!playback.open(playbackDevice, SND_PCM_STREAM_PLAYBACK)) {
        std::fprintf(stderr, "odt_test: failed to open audio output device %s\n", playbackDevice.c_str());
        return 1;
    }

    dextra::AlsaPcm capture;
    if (doTx && !capture.open(captureDevice, SND_PCM_STREAM_CAPTURE)) {
        std::fprintf(stderr, "odt_test: failed to open audio input device %s\n", captureDevice.c_str());
        return 1;
    }

    dextra::PcmQueue rxQueue;

    dmr::rewind::RewindClient client;
    if (!client.open(host)) {
        std::fprintf(stderr, "odt_test: failed to open socket to %s\n", host.c_str());
        return 1;
    }
    client.setIdentity(dmrId, password, "DV3000Client odt_test spike");

    client.setSuperHeaderSink([](const dmr::rewind::SuperHeaderInfo &h) {
        std::fprintf(stderr, "odt_test: SuperHeader type=%u src=%u(%s) dst=%u(%s)\n", h.sessionType, h.source,
                      h.sourceCall.c_str(), h.target, h.targetCall.c_str());
    });
    client.setDmrDataSink([](uint8_t dataType, const uint8_t *data, size_t len) {
        std::fprintf(stderr, "odt_test: DMRData type=%#04x len=%zu:", dataType, len);
        for (size_t i = 0; i < len && i < 16; i++) std::fprintf(stderr, " %02x", data[i]);
        std::fprintf(stderr, "%s\n", len > 16 ? " ..." : "");
    });
    client.setVoiceRxSink(dmr::makeVoiceRxHandler(&dv, &rxQueue));

    if (client.link() != dmr::LinkResult::Success) {
        std::fprintf(stderr, "odt_test: handshake failed\n");
        return 1;
    }

    if (doTx)
        client.subscribe(txTalkgroup, txPrivate ? dmr::rewind::SessionType::PrivateVoice : dmr::rewind::SessionType::GroupVoice);
    for (int i = rxArgStart; i < argc; i++) {
        uint32_t tg = static_cast<uint32_t>(std::strtoul(argv[i], nullptr, 10));
        if (tg > 0) client.subscribe(tg, dmr::rewind::SessionType::GroupVoice);
    }

    std::thread playbackThread(dmr::playbackThread, &playback, &rxQueue);

    std::atomic<bool> pttActive{false};
    std::thread captureThread;
    std::thread pttInputThread;
    if (doTx) {
        captureThread = std::thread(
            dmr::captureThread, &dv, &capture, &client, [txTalkgroup] { return txTalkgroup; },
            [txPrivate] { return txPrivate ? dmr::CallType::Private : dmr::CallType::Group; },
            [&pttActive] { return pttActive.load(); });
        pttInputThread = std::thread([&pttActive, txTalkgroup, txPrivate] {
            std::fprintf(stderr, "odt_test: press Enter to toggle PTT (TG %u, %s)\n", txTalkgroup,
                         txPrivate ? "private" : "group");
            std::string line;
            while (dmr::g_running && std::getline(std::cin, line)) {
                bool active = !pttActive.load();
                pttActive.store(active);
                std::fprintf(stderr, "odt_test: PTT %s\n", active ? "DOWN" : "up");
            }
        });
    }

    std::fprintf(stderr, "odt_test: running (Ctrl+C to stop)\n");
    client.run();
    client.disconnect();

    if (doTx) {
        pttActive.store(false);
        captureThread.join();
        // getline() on stdin has no clean way to be interrupted from
        // another thread -- detach rather than block process exit on
        // the user pressing Enter one more time.
        pttInputThread.detach();
    }
    playbackThread.join();

    playback.close();
    if (doTx) capture.close();
    dv.close();
    return 0;
}
