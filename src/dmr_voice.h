#pragma once

// DMRD voice-burst framing: the actual over-the-air voice packet format,
// as distinct from dmr_client.h's connect/login/config handshake. A DMR
// voice burst's 33-byte payload isn't just raw AMBE like D-Star's frames
// are -- it's BPTC(196,96)-encoded Link Control in the header/terminator,
// and embedded signaling (Golay/QR/Hamming-encoded) woven into the sync
// gap of each voice frame, on top of the two AMBE+2 half-rate frames the
// burst actually carries. Ported from xlxd's cdmrmmdvmprotocol.cpp
// (EncodeDvHeaderPacket/EncodeDvPacket/EncodeDvLastPacket/
// AppendVoiceLCToBuffer/AppendTerminatorLCToBuffer/ReplaceEMBInBuffer/
// EncodeEmbeddedLC), reusing the same FEC primitives (dmr_fec, itself
// reused directly from xlxd -- see CMakeLists.txt) it does, translated
// from xlxd's CBuffer/CCallsign API to this project's plain
// std::vector<uint8_t> packet-building style. The exact offset arithmetic
// is preserved as faithfully as possible rather than restructured, since
// it encodes the real DMR TDMA burst bit layout, not an arbitrary choice.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dmr {

constexpr size_t DMRD_PACKET_SIZE = 55;
constexpr size_t AMBE_FRAME_SIZE = 9; // one AMBE+2 half-rate frame (72 bits)

// The embedded LC fragments broadcast piecemeal across voice frames B-E of
// every burst in a transmission -- constant for the transmission's
// duration, computed once from the source ID.
using EmbeddedLC = std::array<uint8_t, 16>;
EmbeddedLC encodeEmbeddedLC(uint32_t srcId);

// A 55-byte DMRD header packet: BPTC-encoded Voice LC Header, sent once at
// the start of a transmission before any voice frames.
std::vector<uint8_t> buildHeaderFrame(uint32_t srcId, uint32_t dstId, uint32_t rptrId, uint32_t streamId,
                                       uint8_t seqId);

// A 55-byte DMRD voice packet carrying one full DMR voice burst (3 AMBE
// half-rate frames). frameInBurst cycles 0-5 across consecutive bursts
// within a transmission (A-F in DMR terms): 0 is VOICE SYNC (a known sync
// pattern instead of embedded signaling, marks a resync point), 1-4 each
// carry one quarter of embeddedLC, 5 carries a fixed null EMB (no LC
// fragment -- there are only 4 quarters, and F is a natural period-6 tail).
std::vector<uint8_t> buildVoiceFrame(uint32_t srcId, uint32_t dstId, uint32_t rptrId, uint32_t streamId,
                                      uint8_t seqId, int frameInBurst, const uint8_t ambe0[AMBE_FRAME_SIZE],
                                      const uint8_t ambe1[AMBE_FRAME_SIZE], const uint8_t ambe2[AMBE_FRAME_SIZE],
                                      const EmbeddedLC &embeddedLC);

// A 55-byte DMRD terminator packet: BPTC-encoded Terminator-with-LC, ends
// a transmission.
std::vector<uint8_t> buildTerminatorFrame(uint32_t srcId, uint32_t dstId, uint32_t rptrId, uint32_t streamId,
                                           uint8_t seqId);

// RX: true if `packet` (DMRD_PACKET_SIZE bytes) is a voice frame (sync or
// not -- both carry the same 3 AMBE frames, just different embedded
// signaling), with the 3 AMBE half-rate frames extracted. No FEC decode
// needed here -- unlike the header/terminator/embedded-signaling fields,
// the AMBE payload itself isn't BPTC/Golay/QR-protected at this layer, so
// this is just byte-slicing the same positions buildVoiceFrame() writes.
bool extractVoiceFrame(const uint8_t *packet, size_t length, uint8_t ambe0[AMBE_FRAME_SIZE],
                        uint8_t ambe1[AMBE_FRAME_SIZE], uint8_t ambe2[AMBE_FRAME_SIZE]);

} // namespace dmr
