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
    std::string loginCallsign = callsign;
    if (sep != std::string::npos && sep + 1 < full.size()) {
        loginCallsign.resize(7, ' ');
        loginCallsign += full.back();
    }
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

    dplus::DPlusClient client;
    if (!client.open(host) || !client.link(loginCallsign)) return 1;
    if (kerchunk) client.sendKerchunk(upper(target), wanted, callsign);
    std::fprintf(stderr, "dplus_test: linked; playing %s; Ctrl-C to stop\n",
                 wanted == '*' ? "whichever stream comes first (all modules)" : (std::string("module ") + wanted).c_str());

    std::unique_ptr<audio::VocoderPipeline> vocoder;
    if (!logOnly) vocoder = std::make_unique<audio::VocoderPipeline>(dv, SerialDV::DVRate3600x2400);
    std::map<uint16_t, Stream> streams;
    int playing = -1; // stream ID being played, -1 if none

    client.setHeaderSink([&](uint16_t id, const dextra::DStarHeader &h) {
        auto now = std::chrono::steady_clock::now();
        // A played stream that went quiet without its last frame is over.
        if (playing >= 0 && (!streams.count(playing) || now - streams[playing].last > std::chrono::seconds(1))) playing = -1;
        // The module is in RPT2 ("REF001 C", or "ZL2VH  B" for traffic the
        // gateway sends towards module B) -- unless RPT2 is the gateway
        // itself ("ZL2VH  G", a radio keying the repeater), when it's in
        // RPT1 ("ZL2VH  C"). RPT1 on a reflector is the sender's own
        // hotspot ("KI4EYK D"), so it can't come first.
        char m1 = h.rpt1.empty() ? ' ' : h.rpt1.back(), m2 = h.rpt2.empty() ? ' ' : h.rpt2.back();
        char module = m2 != 'G' && m2 != ' ' ? m2 : m1;
        bool play = playing < 0 && (wanted == '*' || module == wanted);
        if (play) playing = id;
        streams[id] = Stream{h.myCall + (h.myCall2.empty() ? "" : "/" + h.myCall2), module, 0, now, now};
        std::fprintf(stderr, "dplus_test: stream %u started: MY %-8s/%-4s UR %-8s RPT1 %-8s RPT2 %-8s module %c%s\n", id,
                     h.myCall.c_str(), h.myCall2.c_str(), h.urCall.c_str(), h.rpt1.c_str(), h.rpt2.c_str(), module,
                     play ? "  [playing]" : (playing >= 0 ? "  [not played: another stream is playing]" : "  [not played: other module]"));
    });
    client.setFrameSink([&](uint16_t id, uint8_t, const uint8_t *ambe, bool last) {
        auto it = streams.find(id);
        if (it == streams.end()) return; // joined mid-stream, header not seen
        it->second.last = std::chrono::steady_clock::now();
        if (!last) {
            it->second.frames++;
            if (id == playing && vocoder) vocoder->decode(ambe, [&queue](const short *pcm) { queue.push(pcm); });
            return;
        }
        double secs = std::chrono::duration<double>(it->second.last - it->second.start).count();
        std::fprintf(stderr, "dplus_test: stream %u ended: %s, module %c, %d frames over %.1f s\n", id,
                     it->second.who.c_str(), it->second.module, it->second.frames, secs);
        if (id == playing) playing = -1;
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
