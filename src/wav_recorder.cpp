#include "wav_recorder.h"

#include <cstdint>

namespace audio {

namespace {

constexpr uint32_t kSampleRate = 8000;
constexpr uint16_t kBitsPerSample = 16;
constexpr uint16_t kChannels = 1;

void put16(FILE *f, uint16_t v) {
    unsigned char b[2] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8)};
    std::fwrite(b, 1, 2, f);
}

void put32(FILE *f, uint32_t v) {
    unsigned char b[4] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8),
                          static_cast<unsigned char>(v >> 16), static_cast<unsigned char>(v >> 24)};
    std::fwrite(b, 1, 4, f);
}

} // namespace

bool WavRecorder::start(const std::string &path) {
    stop();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_file = std::fopen(path.c_str(), "wb");
    if (!m_file) return false;
    m_dataBytes = 0;
    writeHeader(); // sizes are filled in by stop()
    return true;
}

void WavRecorder::stop() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_file) return;
    std::rewind(m_file);
    writeHeader();
    std::fclose(m_file);
    m_file = nullptr;
}

bool WavRecorder::recording() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_file != nullptr;
}

void WavRecorder::write(const short *samples, size_t count) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_file) return;
    // WAV is little-endian, as are the PCs and Raspberry Pis this runs on.
    m_dataBytes += std::fwrite(samples, sizeof(short), count, m_file) * sizeof(short);
}

void WavRecorder::writeHeader() {
    put32(m_file, 0x46464952); // "RIFF"
    put32(m_file, static_cast<uint32_t>(36 + m_dataBytes));
    put32(m_file, 0x45564157); // "WAVE"
    put32(m_file, 0x20746d66); // "fmt "
    put32(m_file, 16);
    put16(m_file, 1); // PCM
    put16(m_file, kChannels);
    put32(m_file, kSampleRate);
    put32(m_file, kSampleRate * kChannels * kBitsPerSample / 8);
    put16(m_file, kChannels * kBitsPerSample / 8);
    put16(m_file, kBitsPerSample);
    put32(m_file, 0x61746164); // "data"
    put32(m_file, static_cast<uint32_t>(m_dataBytes));
}

} // namespace audio
