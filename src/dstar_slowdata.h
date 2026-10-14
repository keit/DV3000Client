#pragma once

// D-Star slow data: the 3 bytes after each voice frame's 9 AMBE bytes. A
// transmission is a run of 21-frame superframes; frame 0 of each carries
// the sync pattern, the other 20 carry data, scrambled by XOR with
// 70 4F 93. What we send there is the "TX message" radios have (shown on
// other radios and on dashboards): 20 characters as 4 blocks of 5, each
// block a type byte (0x40 + block number) and its 5 characters, 6 bytes
// over two frames -- in frames 1..8 of every superframe, so a station that
// tunes in partway through still gets it. The rest is idle filler (0x66
// scrambled). Format as in G4KLX's ircDDBGateway/MMDVMHost (CSlowDataEncoder).

#include <cstdint>
#include <cstring>
#include <string>

namespace dstar {

constexpr uint8_t SLOW_DATA_SYNC[3] = {0x55, 0x2D, 0x16};
constexpr uint8_t SLOW_DATA_FILLER[3] = {0x16, 0x29, 0xF5};
constexpr int TX_MESSAGE_LENGTH = 20;

// The slow data bytes for frame packetId (0..20) of a transmission whose
// TX message is message (blank: none; longer than 20 is cut).
inline void slowData(uint8_t packetId, const std::string &message, uint8_t out[3]) {
    if (packetId == 0) {
        std::memcpy(out, SLOW_DATA_SYNC, 3);
        return;
    }
    if (message.find_first_not_of(' ') == std::string::npos || packetId > 8) {
        std::memcpy(out, SLOW_DATA_FILLER, 3);
        return;
    }
    std::string text = message.substr(0, TX_MESSAGE_LENGTH);
    text.resize(TX_MESSAGE_LENGTH, ' ');
    int block = (packetId - 1) / 2;
    uint8_t bytes[6] = {static_cast<uint8_t>(0x40 + block)};
    std::memcpy(bytes + 1, text.data() + block * 5, 5);
    const uint8_t *half = bytes + ((packetId - 1) % 2) * 3;
    const uint8_t scramble[3] = {0x70, 0x4F, 0x93};
    for (int i = 0; i < 3; i++) out[i] = half[i] ^ scramble[i];
}

// The other way: collects a received transmission's TX message from its
// frames' slow data. Lost packets only cost the block they were part of;
// it's repeated every superframe.
class SlowDataDecoder {
public:
    void reset() {
        m_text.assign(TX_MESSAGE_LENGTH, ' ');
        m_blocks = 0;
        m_reported = false;
        m_firstHalfId = -1;
    }
    // Returns true once, when all 4 blocks of a message are in; message()
    // is then the text, trailing spaces removed.
    bool add(uint8_t packetId, const uint8_t data[3]) {
        packetId &= 0x1F; // without the last-frame flag
        if (packetId == 0 || packetId > 20) return false; // sync
        const uint8_t scramble[3] = {0x70, 0x4F, 0x93};
        uint8_t d[3];
        for (int i = 0; i < 3; i++) d[i] = data[i] ^ scramble[i];
        if (packetId % 2 == 1) { // a block's first half
            std::memcpy(m_firstHalf, d, 3);
            m_firstHalfId = packetId;
            return false;
        }
        if (m_firstHalfId != packetId - 1) return false; // first half lost
        m_firstHalfId = -1;
        uint8_t type = m_firstHalf[0];
        if ((type & 0xF0) != 0x40 || (type & 0x0F) > 3) return false; // not a text block
        int block = type & 0x0F;
        char chars[5] = {static_cast<char>(m_firstHalf[1]), static_cast<char>(m_firstHalf[2]),
                         static_cast<char>(d[0]), static_cast<char>(d[1]), static_cast<char>(d[2])};
        for (char &c : chars) {
            if (c < ' ' || c > '~') c = ' ';
        }
        m_text.replace(static_cast<size_t>(block) * 5, 5, chars, 5);
        m_blocks |= 1 << block;
        if (m_blocks != 0x0F || m_reported) return false;
        m_reported = true;
        return true;
    }
    std::string message() const {
        size_t end = m_text.find_last_not_of(' ');
        return end == std::string::npos ? std::string() : m_text.substr(0, end + 1);
    }

private:
    std::string m_text = std::string(TX_MESSAGE_LENGTH, ' ');
    int m_blocks = 0;
    bool m_reported = false;
    uint8_t m_firstHalf[3] = {};
    int m_firstHalfId = -1;
};

} // namespace dstar
