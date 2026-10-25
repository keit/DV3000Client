#pragma once

// YSF live mode's TX: the capture thread that pumps mic PCM through the
// ThumbDV into a YsfClient session -- the YSF counterpart of dextra_audio.h,
// with its own running flag, which also stops the session's playback and
// network threads.

#include <csignal>
#include <cstdint>
#include <functional>

#include "alsa_audio.h"
#include "vocoder_pipeline.h"
#include "ysf_client.h"

namespace ysf {

// Cleared to unwind a YSF session's threads.
extern volatile std::sig_atomic_t g_running;

// PTT-driven mic -> ThumbDV -> network, on its own thread, paced by
// AlsaPcm::read(). vocoder must be at DMR's rate (DVRate3600x2450, which
// is YSF DN mode's); encoded frames go to the client from its reply
// thread. silence: an AMBE frame encoded from silent audio, to fill out the
// last YSF frame of a transmission.
void captureThread(audio::VocoderPipeline *vocoder, audio::AlsaPcm *capture, YsfClient *client,
                   const uint8_t *silence, std::function<bool()> pttActive);

} // namespace ysf
