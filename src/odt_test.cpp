// Standalone spike CLI for BrandMeister's Open DMR Terminal (Rewind)
// protocol -- connects, authenticates, subscribes to one or more
// talkgroups, and plays received voice back through a real ThumbDV +
// ALSA, so the real wire behaviour can be heard and cross-checked
// against github.com/BrandMeister/go-brandmeister and
// github.com/BrandMeister/DigestPlay before any of this is wired into
// the GUI. See dmr_rewind.h's header comment for what's confirmed vs.
// still unverified (in particular: no voice TX yet, this is RX-only).
//
// Usage: odt_test <host> <dmrId> <password> <thumbdv-device> <playback-device> [talkgroup ...]
//   dmrId must be your real DMR ID here, not an arbitrary application ID
//   -- confirmed against a live master that BrandMeister ties the
//   RemoteID declared in the initial KeepAlive to the account the
//   password belongs to; an unrelated placeholder value gets a
//   correctly-authenticated session silently dropped.
//   password is BrandMeister's "Hotspot Security" password from
//   SelfCare, the same one an existing Homebrew/MMDVM connection
//   already uses.

#include "dextra_audio.h" // AlsaPcm, PcmQueue
#include "dmr_audio.h"    // makeVoiceRxHandler, playbackThread
#include "dmr_rewind.h"
#include "dmr_transport.h" // dmr::g_running
#include "dvcontroller.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {
void handleSignal(int) { dmr::g_running = 0; }
} // namespace

int main(int argc, char **argv) {
    if (argc < 6) {
        std::fprintf(stderr, "usage: %s <host> <dmrId> <password> <thumbdv-device> <playback-device> [talkgroup ...]\n",
                      argv[0]);
        return 1;
    }
    std::string host = argv[1];
    uint32_t dmrId = static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 10));
    std::string password = argv[3];
    std::string thumbdvDevice = argv[4];
    std::string playbackDevice = argv[5];

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

    dextra::PcmQueue rxQueue;
    auto voiceRxHandler = dmr::makeVoiceRxHandler(&dv, &rxQueue);

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
    // 27 bytes = 3 AMBE+2 half-rate frames (9 bytes each), one 60ms voice
    // burst -- confirmed live against a real master, matching exactly
    // what BrandMeister's own DigestPlay reference sends
    // (MODE33_FRAME_SIZE * 3). Anything else is unexpected -- logged,
    // not decoded, rather than guessing at a different frame layout.
    client.setDmrAudioSink([&voiceRxHandler](uint8_t subType, const uint8_t *data, size_t len) {
        if (len != 3 * dmr::AMBE_FRAME_SIZE) {
            std::fprintf(stderr, "odt_test: DMRAudio subType=%u unexpected len=%zu (expected %zu), skipping\n", subType,
                          len, 3 * dmr::AMBE_FRAME_SIZE);
            return;
        }
        voiceRxHandler(data, data + dmr::AMBE_FRAME_SIZE, data + 2 * dmr::AMBE_FRAME_SIZE);
    });

    if (!client.link()) {
        std::fprintf(stderr, "odt_test: handshake failed\n");
        return 1;
    }

    for (int i = 6; i < argc; i++) {
        uint32_t tg = static_cast<uint32_t>(std::strtoul(argv[i], nullptr, 10));
        if (tg > 0) client.subscribe(tg, dmr::rewind::SessionType::GroupVoice);
    }

    std::thread playbackThread(dmr::playbackThread, &playback, &rxQueue);

    std::fprintf(stderr, "odt_test: running (Ctrl+C to stop)\n");
    client.run();
    client.disconnect();

    playbackThread.join();
    playback.close();
    dv.close();
    return 0;
}
