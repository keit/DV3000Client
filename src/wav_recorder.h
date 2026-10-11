#pragma once

// Records what's played to a WAV file (8 kHz, 16-bit, mono -- the DV
// audio format), for the Record button in the Speaker box. The playback
// thread writes every frame it plays; the GUI starts and stops recording
// at any time, from its own thread.

#include <cstdio>
#include <mutex>
#include <string>

namespace audio {

class WavRecorder {
public:
    ~WavRecorder() { stop(); }

    // Starts a new file at path (stopping any current one first); false if
    // it can't be created.
    bool start(const std::string &path);
    // Finishes the file: fills in the header's sizes and closes it.
    void stop();
    bool recording() const;

    // Appends samples if recording, otherwise does nothing.
    void write(const short *samples, size_t count);

private:
    void writeHeader(); // with m_mutex held

    mutable std::mutex m_mutex;
    FILE *m_file = nullptr;
    unsigned long m_dataBytes = 0;
};

} // namespace audio
