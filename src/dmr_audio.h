#pragma once

// ALSA audio I/O for DMR live mode -- reuses dextra_audio's AlsaPcm/PcmQueue
// as-is (they're already protocol-agnostic: 8kHz/16-bit-mono, one period
// per 20ms DV frame, regardless of which vocoder rate that frame gets
// encoded at) but needs its own capture/playback threads, since DMR's
// voice burst groups three 20ms AMBE+2 half-rate frames into one 60ms
// DMRD packet -- D-Star sends one AMBE frame per packet, so
// dextra::captureThread/playbackThread's 1:1 framing doesn't fit.
//
// Kept independent of dextra_client.h/dextra_audio.cpp's threads and
// g_running, same reasoning as dmr_client.h staying independent of
// DextraClient: one physical ThumbDV can only serve one live session at a
// time, but nothing here should assume the D-Star and DMR sides share a
// lifecycle.

#include <functional>
#include <mutex>

#include "dextra_audio.h" // AlsaPcm, PcmQueue
#include "dmr_transport.h"
#include "dvcontroller.h"

namespace dmr {

// Guards concurrent access to the DVController shared between this
// session's capture thread (encode) and DmrClient::run()'s thread (decode,
// via the sink installed by makeVoiceRxHandler) -- same role as
// dextra::g_dvMutex, just a separate instance since DMR and D-Star never
// share a DVController.
extern std::mutex g_dvMutex;

// Live-mode TX: PTT-driven mic -> ThumbDV -> DmrTransport, mirroring
// dextra::captureThread's shape and fast-read throttling, but accumulating
// three 20ms AMBE+2 half-rate frames into each 60ms voice burst before
// calling sendVoiceFrame. talkgroup and callType are each read
// once per transmission (at the PTT-down edge), not per frame, so
// changing either mid-transmission doesn't retarget an in-flight stream.
void captureThread(SerialDV::DVController *dv, dextra::AlsaPcm *capture, DmrTransport *client,
                    std::function<uint32_t()> talkgroup, std::function<dmr::CallType()> callType,
                    std::function<bool()> pttActive);

// Builds a DmrClient::setVoiceRxSink callback: decodes each of a burst's 3
// AMBE half-rate frames back to PCM and pushes them onto queue, in order,
// for playbackThread to drain.
std::function<void(const uint8_t *, const uint8_t *, const uint8_t *)>
makeVoiceRxHandler(SerialDV::DVController *dv, dextra::PcmQueue *queue);

// Live-mode RX playback -- identical idle/backfill behavior to
// dextra::playbackThread (see its comments for why), just paced by
// dmr::g_running instead of dextra::g_running so the two sides' threads
// can be stopped independently.
void playbackThread(dextra::AlsaPcm *playback, dextra::PcmQueue *queue);

} // namespace dmr
