#pragma once

// D-Star live-mode TX: the capture thread that pumps mic PCM through the
// ThumbDV into a DextraClient session. The protocol-independent ALSA
// pieces (streams, RX queue, playback thread) live in alsa_audio.h. Kept
// separate from dextra_client.h/.cpp -- the DExtra protocol client has no
// business depending on ALSA specifically, and a frontend that wants a
// different audio backend (or none at all, e.g. file-based test modes)
// shouldn't have to link this.

#include <functional>

#include "alsa_audio.h"
#include "dextra_client.h"
#include "vocoder_pipeline.h"

namespace dextra {

// Live-mode TX: PTT-driven mic -> ThumbDV -> network. Runs on its own
// thread; AlsaPcm::read() blocking for one ALSA period (20ms) is what
// paces it. pttActive is polled once per period to decide whether to
// originate/continue/end a transmission -- callers supply it (a terminal
// key state, a GUI button, ...) rather than this assuming any particular
// input device. vocoder must be at DVRate3600x2400 (D-Star's AMBE);
// frames are sent from its reply thread as they come back encoded.
void captureThread(audio::VocoderPipeline *vocoder, audio::AlsaPcm *capture, DextraClient *client,
                    std::function<bool()> pttActive);

} // namespace dextra
