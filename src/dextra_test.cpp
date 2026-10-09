// Step 4 of the ThumbDV/XLX client build order: wire up a real microphone
// and speaker. Adds a "live" mode that plays back received audio to a
// speaker in real time (decode-on-arrival, no buffering) and transmits
// mic audio, encoded through the ThumbDV, while Space is held -- on top
// of (not replacing) the existing file-based echo/test modes, which stay
// as the regression-test path for automated/offline verification.
//
// The DExtra protocol and ThumbDV/ALSA session logic live in
// dextra_client.h/.cpp, shared with other frontends (e.g. a GUI). This
// file is the CLI-only bits: argument parsing, terminal-based PTT, and
// the XLX reflector directory lookup for turning a query like "XLX123"
// into a host.
//
// Usage: dextra_test <host> <target module letter> <tty device> [test <input.raw> | <rx_output.raw> | live [capture-dev [playback-dev]]]

#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <alsa/asoundlib.h>
#include <termios.h>
#include <unistd.h>

#include "dextra_audio.h"
#include "dextra_client.h"
#include "xlx_directory.h"

namespace {

void onSignal(int) { dextra::g_running = 0; }

// Push-to-talk via a held key. A plain TTY has no physical key-up event --
// holding a key just produces repeated bytes from the OS's keyboard
// auto-repeat -- so "held" is approximated the standard way: PTT is
// considered active from the first Space byte until PTT_HANGTIME_MS passes
// with no further Space byte, functionally the same as a VOX hangtime but
// gated on the key rather than mic level.
constexpr int PTT_HANGTIME_MS = 200;

class PttInput {
public:
    bool start() {
        if (tcgetattr(STDIN_FILENO, &m_savedTermios) != 0) return false;
        struct termios raw = m_savedTermios;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 1; // 100ms read timeout, in tenths of a second
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return false;
        m_termiosSaved = true;

        std::fprintf(stderr, "dextra_test: hold SPACE to transmit, Ctrl+C to quit\n");
        m_thread = std::thread(&PttInput::run, this);
        return true;
    }

    void stop() {
        if (m_thread.joinable()) m_thread.join();
        if (m_termiosSaved) tcsetattr(STDIN_FILENO, TCSANOW, &m_savedTermios);
    }

    bool active() const { return m_active.load(); }

private:
    void run() {
        auto lastSpace = std::chrono::steady_clock::now() - std::chrono::hours(1);
        while (dextra::g_running) {
            char c;
            ssize_t n = ::read(STDIN_FILENO, &c, 1);
            auto now = std::chrono::steady_clock::now();
            if (n == 1 && c == ' ') {
                lastSpace = now;
                m_active.store(true);
            }
            if (m_active.load() &&
                std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSpace).count() > PTT_HANGTIME_MS) {
                m_active.store(false);
            }
        }
        m_active.store(false);
    }

    std::atomic<bool> m_active{false};
    std::thread m_thread;
    struct termios m_savedTermios{};
    bool m_termiosSaved = false;
};

} // namespace

void usage(const char *prog) {
    std::fprintf(stderr, "usage: %s <host> <target module letter> <tty device> [test <input.raw> | <rx_output.raw> | live [capture-dev [playback-dev [rx_output.raw]]]]\n", prog);
    std::fprintf(stderr, "  no extra arg: link and echo received transmissions back through the ThumbDV\n");
    std::fprintf(stderr, "  <rx_output.raw>: also save decoded RX PCM (8kHz/16-bit-LE) there for inspection\n");
    std::fprintf(stderr, "  'test' <input.raw>: encode that PCM file via the ThumbDV and send it as one "
                          "transmission, then exit (needs a second instance running to observe the echo "
                          "-- a reflector never relays a stream back to its own originator)\n");
    std::fprintf(stderr, "  'live': hold SPACE to transmit captured mic audio, and play back received audio "
                          "in real time. capture-dev/playback-dev default to ALSA's \"default\" PCM; "
                          "run `arecord -l` / `aplay -l` to see actual device names on this machine. "
                          "rx_output.raw there too saves decoded RX PCM (8kHz/16-bit-LE) alongside live playback\n");
}

// If `arg` looks like a reflector query -- purely alphanumeric, e.g. "123",
// "XLX123", or "XRF123" -- resolve it against the live XLX directory
// (falling back to the bundled static XRF list at data/DExtra_Hosts.txt for
// reflectors that never migrated to xlxd). A dotted-quad IP or a hostname
// with dots never matches this shape, so existing invocations like
// "127.0.0.1" are returned unchanged. If the query doesn't resolve to
// anything (network failure, unknown number, or it wasn't really a query --
// e.g. a bare hostname with no digits), `arg` is returned unchanged and
// gets tried directly as a host, exactly like before this lookup existed.
std::string resolveReflectorHost(const std::string &arg) {
    bool looksLikeQuery = !arg.empty();
    for (char c : arg) {
        if (!std::isalnum(static_cast<unsigned char>(c))) { looksLikeQuery = false; break; }
    }
    if (!looksLikeQuery) return arg;

    std::vector<xlx::ReflectorInfo> live;
    std::string error;
    if (!xlx::fetchReflectorList(live, error)) {
        std::fprintf(stderr, "dextra_test: could not fetch XLX reflector list (%s); "
                              "trying '%s' as a literal host\n", error.c_str(), arg.c_str());
        return arg;
    }
    std::vector<xlx::ReflectorInfo> fallback;
    xlx::loadStaticFallback("data/DExtra_Hosts.txt", fallback);

    const xlx::ReflectorInfo *found = xlx::findReflector(live, fallback, arg);
    if (!found) return arg;

    std::fprintf(stderr, "dextra_test: resolved '%s' -> %s (%s)\n",
                 arg.c_str(), found->name.c_str(), found->host.c_str());
    return found->host;
}

int main(int argc, char **argv) {
    if (argc < 4 || argc > 8) {
        usage(argv[0]);
        return 1;
    }

    bool sendTest = false;
    bool liveMode = false;
    std::string testPcmPath, rxOutPath, liveRxOutPath;
    std::string captureDev = "default", playbackDev = "default";

    if (argc == 4) {
        // file-based echo mode, no extra arg
    } else if (std::strcmp(argv[4], "test") == 0) {
        if (argc != 6) { usage(argv[0]); return 1; }
        sendTest = true;
        testPcmPath = argv[5];
    } else if (std::strcmp(argv[4], "live") == 0) {
        liveMode = true;
        if (argc >= 6) captureDev = argv[5];
        if (argc >= 7) playbackDev = argv[6];
        if (argc >= 8) liveRxOutPath = argv[7];
    } else if (argc == 5) {
        rxOutPath = argv[4];
    } else {
        usage(argv[0]);
        return 1;
    }

    SerialDV::DVController dv;
    if (!dv.open(argv[3])) {
        std::fprintf(stderr, "dextra_test: failed to open %s\n", argv[3]);
        return 1;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    if (liveMode) {
        audio::AlsaPcm capture, playback;
        if (!capture.open(captureDev, SND_PCM_STREAM_CAPTURE)) return 1;
        if (!playback.open(playbackDev, SND_PCM_STREAM_PLAYBACK)) return 1;

        FILE *liveRxPcmOut = nullptr;
        if (!liveRxOutPath.empty()) {
            liveRxPcmOut = std::fopen(liveRxOutPath.c_str(), "wb");
            if (!liveRxPcmOut) {
                std::fprintf(stderr, "dextra_test: cannot open %s for writing\n", liveRxOutPath.c_str());
                return 1;
            }
        }

        audio::PcmQueue rxQueue;
        dextra::DextraClient client(&dv, [&rxQueue](const short *pcm) { rxQueue.push(pcm); }, liveRxPcmOut);
        // After client, so it's destroyed first: its replies call into client.
        audio::VocoderPipeline vocoder(dv, SerialDV::DVRate3600x2400);
        client.setVocoder(&vocoder);
        if (!client.open(resolveReflectorHost(argv[1]), argv[2][0])) return 1;
        if (!client.link()) return 1;

        PttInput ptt;
        if (!ptt.start()) {
            std::fprintf(stderr, "dextra_test: failed to set up PTT input (not a terminal?)\n");
            return 1;
        }

        std::thread capThread(dextra::captureThread, &vocoder, &capture, &client, [&ptt] { return ptt.active(); });
        std::thread playThread(audio::playbackThread, &playback, &rxQueue, &dextra::g_running);

        client.run(); // blocks until g_running is cleared (SIGINT/SIGTERM)

        capThread.join();
        playThread.join();
        ptt.stop();
        capture.close();
        playback.close();

        client.disconnect();
        if (liveRxPcmOut) std::fclose(liveRxPcmOut);
        dv.close();
        return 0;
    }

    FILE *rxPcmOut = nullptr;
    if (!rxOutPath.empty()) {
        rxPcmOut = std::fopen(rxOutPath.c_str(), "wb");
        if (!rxPcmOut) {
            std::fprintf(stderr, "dextra_test: cannot open %s for writing\n", rxOutPath.c_str());
            return 1;
        }
    }

    dextra::DextraClient client(&dv, rxPcmOut);
    if (!client.open(resolveReflectorHost(argv[1]), argv[2][0])) return 1;
    if (!client.link()) return 1;

    if (sendTest) {
        // One-shot sender: fire the test transmission and exit. Do NOT also
        // run() here -- this instance would then receive and echo its own
        // transmission's echo back, forever, if another instance echoes it
        // (two unconditional "parrot" clients on the same module will always
        // ping-pong; that's real behavior, not a bug, but a one-shot sender
        // should not become one side of that pair by accident).
        client.sendTestTransmission(testPcmPath);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    } else {
        client.run();
    }

    client.disconnect();
    if (rxPcmOut) std::fclose(rxPcmOut);
    dv.close();
    return 0;
}
