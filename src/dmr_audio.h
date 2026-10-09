#pragma once

// DMR live-mode audio: the capture thread and the RX decode handler.
// Uses alsa_audio's AlsaPcm/PcmQueue/playbackThread as-is (they're
// protocol-independent: 8kHz/16-bit-mono, one period per 20ms DV frame,
// whatever vocoder rate that frame gets encoded at), but needs its own
// capture thread, since DMR's voice burst groups three 20ms AMBE+2
// half-rate frames into one 60ms DMRD packet -- D-Star sends one AMBE
// frame per packet, so dextra::captureThread's 1:1 framing doesn't fit.
//
// Kept independent of dextra_client.h/dextra_audio.cpp's threads and
// g_running, same reasoning as dmr_client.h staying independent of
// DextraClient: one physical ThumbDV can only serve one live session at a
// time, but nothing here should assume the D-Star and DMR sides share a
// lifecycle.

#include <functional>

#include "alsa_audio.h"
#include "dmr_transport.h"
#include "vocoder_pipeline.h"

namespace dmr {

// Live-mode TX: PTT-driven mic -> ThumbDV -> DmrTransport, mirroring
// dextra::captureThread's shape (and sharing its audio::CaptureThrottle),
// but accumulating three 20ms AMBE+2 half-rate frames into each 60ms
// voice burst before calling sendVoiceFrame. talkgroup and callType are each read
// once per transmission (at the PTT-down edge), not per frame, so
// changing either mid-transmission doesn't retarget an in-flight stream.
// vocoder must be at DVRate3600x2450 (DMR's AMBE+2); bursts are sent from
// its reply thread as their third frame comes back encoded.
void captureThread(audio::VocoderPipeline *vocoder, audio::AlsaPcm *capture, DmrTransport *client,
                    std::function<uint32_t()> talkgroup, std::function<dmr::CallType()> callType,
                    std::function<bool()> pttActive);

// Builds a DmrClient::setVoiceRxSink callback: submits each of a burst's 3
// AMBE half-rate frames for decoding and returns at once; the PCM is pushed
// onto queue, in order, for playbackThread to drain as the replies come in.
std::function<void(const uint8_t *, const uint8_t *, const uint8_t *)>
makeVoiceRxHandler(audio::VocoderPipeline *vocoder, audio::PcmQueue *queue);

} // namespace dmr
