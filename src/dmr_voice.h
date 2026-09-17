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

// Group (talkgroup) vs Private (unit-to-unit, addressed to another DMR
// ID's radio rather than a talkgroup) call -- affects both the LC content's
// FLCO field (0 = Group, 3 = Unit to Unit, matching MMDVMHost's FLCO enum
// and the DMR air interface spec) and the DMRD packet's own call-type bit
// (0x40 in the bitField byte). Some network features are private-call-only
// -- e.g. BrandMeister's Parrot echo test (TG/ID 9990) only responds to a
// genuine private call, not a group call to that number.
enum class CallType : uint8_t { Group, Private };

// Which of DMR's two TDMA time slots this transmission uses -- affects the
// DMRD packet's own slot bit (0x80 in bitField). Fixed for the whole
// session (see TxParams below), not a per-transmission choice. Slot 2 is
// the confirmed convention for hotspot-style BrandMeister connections
// (verified against a real, working Pi-Star session's own config: "Slot 1:
// disabled / Slot 2: enabled") -- Slot 1 exists for masters/setups that
// expect it instead.
enum class TimeSlot : uint8_t { Slot1, Slot2 };

// Per-connection transmission parameters: color code and time slot are
// fixed for the session (same values declared in the RPTC handshake --
// see RepeaterConfig::colorCode), call type is chosen per-transmission
// (see DmrClient::beginVoiceTx). Bundled into one struct since all three
// now need threading through every frame-building call below.
struct TxParams {
    CallType callType = CallType::Group;
    unsigned colorCode = 1;                    // 0-15
    TimeSlot timeSlot = TimeSlot::Slot2;
};

// The embedded LC fragments broadcast piecemeal across voice frames B-E of
// every burst in a transmission -- constant for the transmission's
// duration, computed once from the source/destination IDs and call type.
// Not affected by color code or time slot (those live in the DMRD packet
// framing and the LC/EMB signaling fields built elsewhere, not in the LC
// content itself), hence the plain CallType parameter rather than TxParams.
using EmbeddedLC = std::array<uint8_t, 16>;
EmbeddedLC encodeEmbeddedLC(uint32_t srcId, uint32_t dstId, CallType callType);

// A 55-byte DMRD header packet: BPTC-encoded Voice LC Header, sent once at
// the start of a transmission before any voice frames.
std::vector<uint8_t> buildHeaderFrame(uint32_t srcId, uint32_t dstId, uint32_t rptrId, uint32_t streamId,
                                       uint8_t seqId, const TxParams &params);

// A 55-byte DMRD voice packet carrying one full DMR voice burst (3 AMBE
// half-rate frames). frameInBurst cycles 0-5 across consecutive bursts
// within a transmission (A-F in DMR terms): 0 is VOICE SYNC (a known sync
// pattern instead of embedded signaling, marks a resync point), 1-4 each
// carry one quarter of embeddedLC, 5 carries a fixed null EMB (no LC
// fragment -- there are only 4 quarters, and F is a natural period-6 tail).
std::vector<uint8_t> buildVoiceFrame(uint32_t srcId, uint32_t dstId, uint32_t rptrId, uint32_t streamId,
                                      uint8_t seqId, int frameInBurst, const uint8_t ambe0[AMBE_FRAME_SIZE],
                                      const uint8_t ambe1[AMBE_FRAME_SIZE], const uint8_t ambe2[AMBE_FRAME_SIZE],
                                      const EmbeddedLC &embeddedLC, const TxParams &params);

// A 55-byte DMRD terminator packet: BPTC-encoded Terminator-with-LC, ends
// a transmission.
std::vector<uint8_t> buildTerminatorFrame(uint32_t srcId, uint32_t dstId, uint32_t rptrId, uint32_t streamId,
                                           uint8_t seqId, const TxParams &params);

// RX: true if `packet` (DMRD_PACKET_SIZE bytes) is a voice frame (sync or
// not -- both carry the same 3 AMBE frames, just different embedded
// signaling), with the 3 AMBE half-rate frames extracted. No FEC decode
// needed here -- unlike the header/terminator/embedded-signaling fields,
// the AMBE payload itself isn't BPTC/Golay/QR-protected at this layer, so
// this is just byte-slicing the same positions buildVoiceFrame() writes.
bool extractVoiceFrame(const uint8_t *packet, size_t length, uint8_t ambe0[AMBE_FRAME_SIZE],
                        uint8_t ambe1[AMBE_FRAME_SIZE], uint8_t ambe2[AMBE_FRAME_SIZE]);

} // namespace dmr
