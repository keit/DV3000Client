// dplus_test: listen to a REF reflector or DPlus gateway through the
// ThumbDV. Logs in to the DPlus authentication server, links, then logs
// every stream's header -- including streams on modules other than the one
// being played, to see what a DPlus client is sent -- and plays one module
// (or whichever stream comes first, with "all").
//
//   dplus_test <callsign> <REF/gateway name, or host/IP> <thumbdv> <playback-device> [module|all] [kerchunk]
//   e.g. dplus_test ZL2MIM ZL2VH /dev/serial/by-id/usb-...ThumbDV... plughw:1,0 C kerchunk
//
// Listen-only, unless "kerchunk" is given: then it transmits 0.2 s of
// silence on that module after linking (see DPlusClient::sendKerchunk).
// With "none" as the ThumbDV, nothing is decoded or played: streams are
// only logged, leaving the vocoder free for another program.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "alsa_audio.h"
#include "dplus_client.h"
#include "vocoder_pipeline.h"

namespace {

volatile std::sig_atomic_t g_running = 1;
void onSignal(int) { g_running = 0; }

std::string upper(std::string s) {
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

struct Stream {
    std::string who;
    char module;
    int frames = 0;
    std::chrono::steady_clock::time_point start;
    std::chrono::steady_clock::time_point last;
};

} // namespace

int main(int argc, char **argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: %s <callsign> <REF/gateway name, or host/IP> <thumbdv> <playback-device> [module|all]\n",
                     argv[0]);
        return 1;
    }
    // "ZL2MIM/B" or "ZL2MIM B": authenticate with the plain callsign, log
    // in as "ZL2MIM B" (callsign padded to 7, the letter 8th) -- as
    // ircDDBGateway/Pi-Star hotspots do, and how one operator runs several
    // radios at once (an IC-705 as "ZL2MIM A", this as "ZL2MIM B").
    std::string full = upper(argv[1]);
    size_t sep = full.find_first_of("/ ");
    std::string callsign = full.substr(0, sep);
    char suffix = sep != std::string::npos && sep + 1 < full.size() ? full.back() : ' ';
    std::string target = argv[2];
    char wanted = argc > 5 && upper(argv[5]) != "ALL" ? static_cast<char>(std::toupper(argv[5][0])) : '*';
    bool kerchunk = argc > 6 && upper(argv[6]) == "KERCHUNK" && wanted != '*';

    std::vector<dplus::GatewayEntry> gateways;
    std::string error;
    if (!dplus::authenticate(callsign, &gateways, error)) {
        std::fprintf(stderr, "dplus_test: authentication failed: %s\n", error.c_str());
        return 1;
    }
    long refs = std::count_if(gateways.begin(), gateways.end(), [](auto &g) { return g.name.rfind("REF", 0) == 0; });
    std::fprintf(stderr, "dplus_test: authenticated as %s; server listed %zu active entries (%ld REF reflectors, %ld gateways)\n",
                 callsign.c_str(), gateways.size(), refs, static_cast<long>(gateways.size()) - refs);

    std::string host = target;
    for (auto &g : gateways) {
        if (upper(g.name) == upper(target)) {
            host = g.address;
            std::fprintf(stderr, "dplus_test: %s is %s (from the server's list)\n", g.name.c_str(), host.c_str());
        }
    }

    bool logOnly = std::string(argv[3]) == "none";
    SerialDV::DVController dv;
    if (!logOnly && !dv.open(argv[3])) {
        std::fprintf(stderr, "dplus_test: cannot open ThumbDV %s\n", argv[3]);
        return 1;
    }
    audio::AlsaPcm playback;
    if (!logOnly && !playback.open(argv[4], SND_PCM_STREAM_PLAYBACK)) return 1;
    audio::PcmQueue queue;

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    std::unique_ptr<audio::VocoderPipeline> vocoder;
    if (!logOnly) vocoder = std::make_unique<audio::VocoderPipeline>(dv, SerialDV::DVRate3600x2400);

    dplus::DPlusClient client;
    client.setIdentity(callsign, suffix);
    client.setTarget(upper(target), wanted);
    if (vocoder) client.setLiveRx(vocoder.get(), [&queue](const short *pcm) { queue.push(pcm); });
    if (!client.open(host)) return 1;
    dplus::LinkResult linked = client.link();
    if (linked != dplus::LinkResult::Accepted && linked != dplus::LinkResult::ReadOnly) return 1;
    if (kerchunk) client.sendKerchunk();
    std::fprintf(stderr, "dplus_test: linked; playing %s; Ctrl-C to stop\n",
                 wanted == '*' ? "whichever stream comes first (all modules)" : (std::string("module ") + wanted).c_str());

    // Log every stream; the client picks which one to play.
    std::map<uint16_t, Stream> streams;
    client.setRawSinks(
        [&](uint16_t id, const dextra::DStarHeader &h) {
            auto now = std::chrono::steady_clock::now();
            char module = dplus::streamModule(h);
            streams[id] = Stream{h.myCall + (h.myCall2.empty() ? "" : "/" + h.myCall2), module, 0, now, now};
            std::fprintf(stderr, "dplus_test: stream %u started: MY %-8s/%-4s UR %-8s RPT1 %-8s RPT2 %-8s module %c\n", id,
                         h.myCall.c_str(), h.myCall2.c_str(), h.urCall.c_str(), h.rpt1.c_str(), h.rpt2.c_str(), module);
        },
        [&](uint16_t id, uint8_t, const uint8_t *, bool last) {
            auto it = streams.find(id);
            if (it == streams.end()) return; // joined mid-stream, header not seen
            it->second.last = std::chrono::steady_clock::now();
            if (!last) {
                it->second.frames++;
                return;
            }
            double secs = std::chrono::duration<double>(it->second.last - it->second.start).count();
            std::fprintf(stderr, "dplus_test: stream %u ended: %s, module %c, %d frames over %.1f s\n", id,
                         it->second.who.c_str(), it->second.module, it->second.frames, secs);
            streams.erase(it);
        });

    std::thread playThread;
    if (!logOnly) playThread = std::thread(audio::playbackThread, &playback, &queue, &g_running, nullptr, nullptr);
    client.run(&g_running);
    g_running = 0;
    client.disconnect();
    if (playThread.joinable()) playThread.join();
    return 0;
}
