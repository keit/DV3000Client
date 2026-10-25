// ysf_test: listen to a YSF reflector through the ThumbDV. Links, logs
// every stream (who, through which gateway, which mode), and plays DN-mode
// audio -- the first step of YSF support, to check the frame decoding
// against real Fusion traffic before any GUI.
//
//   ysf_test <callsign> <reflector name, id, or host[:port]> <thumbdv|none> <playback-device|file.wav|none>
//   e.g. ysf_test ZL2MIM US-America-Link /dev/serial/by-id/usb-...ThumbDV... plughw:2,0
//   ysf_test --list [text]     lists reflectors (matching text)
//   ... --tx speech.wav        transmits the file (8 kHz 16-bit mono WAV) once
//                              linked, then keeps listening -- with a YSF
//                              parrot, e.g. DE-Parrot, to hear it played back
//
// With "none" as the ThumbDV nothing is decoded or played: streams are
// only logged. A .wav file in place of the playback device saves the
// decoded audio instead (8 kHz mono), e.g. to check it while nobody's
// listening.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "alsa_audio.h"
#include "wav_recorder.h"
#include "vocoder_pipeline.h"
#include "ysf_client.h"

namespace {

volatile std::sig_atomic_t g_running = 1;
void onSignal(int) { g_running = 0; }

// The samples of an 8 kHz 16-bit mono WAV file (its "data" chunk).
std::vector<short> readWav(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    for (size_t i = 12; i + 8 <= bytes.size();) {
        uint32_t len;
        std::memcpy(&len, &bytes[i + 4], 4);
        if (std::memcmp(&bytes[i], "data", 4) == 0) {
            size_t n = std::min<size_t>(len, bytes.size() - i - 8) / 2;
            std::vector<short> s(n);
            std::memcpy(s.data(), &bytes[i + 8], n * 2);
            return s;
        }
        i += 8 + len + (len & 1);
    }
    return {};
}

std::string upper(std::string s) {
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

int main(int argc, char **argv) {
    std::vector<ysf::ReflectorEntry> list;
    std::string error;

    if (argc >= 2 && std::string(argv[1]) == "--list") {
        if (!ysf::fetchReflectorList(list, error)) {
            std::fprintf(stderr, "ysf_test: %s\n", error.c_str());
            return 1;
        }
        std::string match = argc > 2 ? upper(argv[2]) : "";
        for (auto &r : list) {
            if (upper(r.name + " " + r.description + " " + r.id).find(match) == std::string::npos) continue;
            std::printf("%-6s %-18s %-24s %s:%d\n", r.id.c_str(), r.name.c_str(), r.description.c_str(),
                        r.host.c_str(), r.port);
        }
        return 0;
    }
    if (argc < 5) {
        std::fprintf(stderr,
                     "usage: %s <callsign> <reflector name, id, or host[:port]> <thumbdv|none> <playback-device|none>\n"
                     "       %s --list [text]\n",
                     argv[0], argv[0]);
        return 1;
    }

    std::string callsign = upper(argv[1]);
    std::string target = argv[2];
    std::string host = target;
    int port = ysf::DEFAULT_PORT;
    size_t colon = target.rfind(':');
    if (colon != std::string::npos) {
        host = target.substr(0, colon);
        port = std::atoi(target.c_str() + colon + 1);
    } else if (ysf::fetchReflectorList(list, error)) {
        for (auto &r : list) {
            if (upper(r.name) == upper(target) || r.id == target) {
                host = r.host;
                port = r.port;
                std::fprintf(stderr, "ysf_test: %s (%s, %s) is %s:%d\n", r.name.c_str(), r.id.c_str(),
                             r.description.c_str(), host.c_str(), port);
            }
        }
    }

    bool logOnly = std::string(argv[3]) == "none";
    SerialDV::DVController dv;
    if (!logOnly && !dv.open(argv[3])) {
        std::fprintf(stderr, "ysf_test: cannot open ThumbDV %s\n", argv[3]);
        return 1;
    }
    std::string output = argv[4];
    bool toFile = output.size() > 4 && output.compare(output.size() - 4, 4, ".wav") == 0;
    bool toDevice = !logOnly && !toFile && output != "none";
    audio::AlsaPcm playback;
    if (toDevice && !playback.open(output, SND_PCM_STREAM_PLAYBACK)) return 1;
    audio::WavRecorder recorder;
    if (!logOnly && toFile && !recorder.start(output)) {
        std::fprintf(stderr, "ysf_test: cannot write %s\n", output.c_str());
        return 1;
    }
    audio::PcmQueue queue;

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    // DN-mode voice is AMBE+2 at DMR's rate.
    std::unique_ptr<audio::VocoderPipeline> vocoder;
    if (!logOnly) vocoder = std::make_unique<audio::VocoderPipeline>(dv, SerialDV::DVRate3600x2450);

    ysf::YsfClient client;
    client.setVoiceSink([&](const uint8_t *ambe) {
        if (vocoder)
            vocoder->decode(ambe, [&](const short *pcm) {
                if (toFile) recorder.write(pcm, SerialDV::MBE_AUDIO_BLOCK_SIZE);
                else queue.push(pcm);
            });
    });
    if (!client.open(host, port) || !client.link(callsign)) return 1;
    std::fprintf(stderr, "ysf_test: linked; Ctrl-C to stop\n");

    if (argc > 6 && std::string(argv[5]) == "--tx" && vocoder) {
        // Encode the whole file first, then send it at the real-time pace
        // of a radio: one YSF frame (5 AMBE frames) every 100 ms.
        std::vector<short> pcm = readWav(argv[6]);
        std::vector<std::vector<uint8_t>> ambe;
        std::mutex lock;
        for (size_t i = 0; i + SerialDV::MBE_AUDIO_BLOCK_SIZE <= pcm.size(); i += SerialDV::MBE_AUDIO_BLOCK_SIZE)
            vocoder->encode(&pcm[i], [&](const uint8_t *a) {
                std::lock_guard<std::mutex> l(lock);
                ambe.emplace_back(a, a + ysf::AMBE_FRAME_SIZE);
            });
        static const short silentPcm[SerialDV::MBE_AUDIO_BLOCK_SIZE] = {};
        std::vector<uint8_t> silence(ysf::AMBE_FRAME_SIZE);
        vocoder->encode(silentPcm, [&](const uint8_t *a) { std::memcpy(silence.data(), a, ysf::AMBE_FRAME_SIZE); });
        vocoder->drain();
        std::fprintf(stderr, "ysf_test: transmitting %s: %zu frames (%.1f s)\n", argv[6], ambe.size(), ambe.size() * 0.02);
        client.beginTx();
        auto next = std::chrono::steady_clock::now();
        for (size_t i = 0; i < ambe.size() && g_running; i++) {
            client.sendVoice(ambe[i].data());
            if (i % 5 == 4) std::this_thread::sleep_until(next += std::chrono::milliseconds(100));
        }
        client.endTx(silence.data());
    }

    std::thread playThread;
    if (toDevice) playThread = std::thread(audio::playbackThread, &playback, &queue, &g_running, nullptr, nullptr);
    client.run(&g_running);
    g_running = 0;
    client.disconnect();
    if (playThread.joinable()) playThread.join();
    vocoder.reset(); // collects what's still in flight before the file is closed
    recorder.stop();
    return 0;
}
