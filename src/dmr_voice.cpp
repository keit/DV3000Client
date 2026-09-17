#include "dmr_voice.h"

#include "cbptc19696.h"
#include "cgolay2087.h"
#include "chamming.h"
#include "cqr1676.h"
#include "crs129.h"
#include "ccrc.h"
#include "cutils.h"

#include <cstring>

namespace dmr {
namespace {

// Fixed DMR sync patterns (48 bits / 6 bytes as transmitted, expressed
// here the same nibble-packed way xlxd's g_DmrSync* constants are: 7
// bytes where byte[0] only uses its low nibble and byte[6] only its high
// nibble, matching where they land in the 33-byte payload -- see
// writeSyncOrEmb() below). MS = mobile-station-originated, which is what
// a repeater/hotspot client sends (it's relaying a subscriber radio's
// transmission to the network) -- xlxd's parser accepts either MS or BS
// sync on receive, but MS is the semantically correct one to originate.
constexpr uint8_t kDmrSyncMSVoice[7] = {0x07, 0xF7, 0xD5, 0xDD, 0x57, 0xDF, 0xD0};
constexpr uint8_t kDmrSyncMSData[7] = {0x0D, 0x5D, 0x7F, 0x77, 0xFD, 0x75, 0x70};

constexpr int DMR_SLOT2_BIT = 0x80; // bitField's slot bit -- 0 = slot 1, 1 = slot 2
constexpr int DMR_PRIVATE_CALL_BIT = 0x40; // bitField's call-type bit -- 0 = group, 1 = private (matches FLCO's meaning)
constexpr uint8_t DMR_DT_VOICE_LC_HEADER = 1;
constexpr uint8_t DMR_DT_TERMINATOR_WITH_LC = 2;
constexpr uint8_t DMR_VOICE_LC_HEADER_CRC_MASK = 0x96;
constexpr uint8_t DMR_TERMINATOR_WITH_LC_CRC_MASK = 0x99;

void appendTag(std::vector<uint8_t> &pkt, const char *tag) { pkt.insert(pkt.end(), tag, tag + std::strlen(tag)); }

// Lower 24 bits, big-endian -- srcId/dstId fields in a DMRD frame.
void appendId24(std::vector<uint8_t> &pkt, uint32_t id) {
    pkt.push_back(static_cast<uint8_t>(id >> 16));
    pkt.push_back(static_cast<uint8_t>(id >> 8));
    pkt.push_back(static_cast<uint8_t>(id));
}

// Full 32 bits, big-endian -- the rptrId field (matches the same 4-byte
// ID used in RPTL/RPTK, unlike the truncated 24-bit srcId/dstId above).
void appendId32(std::vector<uint8_t> &pkt, uint32_t id) {
    pkt.push_back(static_cast<uint8_t>(id >> 24));
    pkt.push_back(static_cast<uint8_t>(id >> 16));
    pkt.push_back(static_cast<uint8_t>(id >> 8));
    pkt.push_back(static_cast<uint8_t>(id));
}

// The common DMRD frame prologue: tag, seqId, srcId(3), dstId(3),
// rptrId(4), bitField(1), streamId(4) -- 20 bytes, offsets 0-19. What
// follows (offsets 20-52, 33 bytes) is the payload, specific to each
// frame type; offsets 53-54 (BER, RSSI) are appended by the caller once
// the payload's in place, matching xlxd's own packet layout exactly.
std::vector<uint8_t> buildPrologue(uint32_t srcId, uint32_t dstId, uint32_t rptrId, uint8_t seqId, uint8_t bitField,
                                    uint32_t streamId) {
    std::vector<uint8_t> pkt;
    pkt.reserve(DMRD_PACKET_SIZE);
    appendTag(pkt, "DMRD");
    pkt.push_back(seqId);
    appendId24(pkt, srcId);
    appendId24(pkt, dstId);
    appendId32(pkt, rptrId);
    pkt.push_back(bitField);
    // Opaque 4-byte token: xlxd passes it through unmodified rather than
    // interpreting it numerically, so only self-consistency (same stream
    // -> same bytes) matters, not a specific endianness.
    pkt.resize(pkt.size() + 4);
    std::memcpy(&pkt[pkt.size() - 4], &streamId, sizeof(streamId));
    return pkt;
}

// Ported from AppendVoiceLCToBuffer/AppendTerminatorLCToBuffer, which are
// identical but for the DT value and CRC mask -- builds the 33-byte
// BPTC(196,96)-encoded Link Control payload used by both the header and
// terminator frames.
std::array<uint8_t, 33> buildLcPayload(uint32_t srcId, uint32_t dstId, uint8_t dtValue, uint8_t crcMask,
                                        CallType callType, unsigned colorCode) {
    std::array<uint8_t, 33> payload{};

    uint8_t lc[12] = {};
    // FLCO: 0 = Group Voice Channel User, 3 = Unit to Unit Voice Channel
    // User (matches MMDVMHost's FLCO enum and the DMR air interface spec)
    // -- not the same numeric value as dmr::CallType, which only needs to
    // distinguish the two cases at this layer's API surface.
    lc[0] = callType == CallType::Private ? 3 : 0;
    lc[3] = static_cast<uint8_t>(dstId >> 16);
    lc[4] = static_cast<uint8_t>(dstId >> 8);
    lc[5] = static_cast<uint8_t>(dstId);
    lc[6] = static_cast<uint8_t>(srcId >> 16);
    lc[7] = static_cast<uint8_t>(srcId >> 8);
    lc[8] = static_cast<uint8_t>(srcId);
    uint8_t parity[4];
    CRS129::encode(lc, 9, parity);
    lc[9] = static_cast<uint8_t>(parity[2] ^ crcMask);
    lc[10] = static_cast<uint8_t>(parity[1] ^ crcMask);
    lc[11] = static_cast<uint8_t>(parity[0] ^ crcMask);

    std::memcpy(payload.data() + 13, kDmrSyncMSData, sizeof(kDmrSyncMSData));

    uint8_t slotType[3] = {};
    slotType[0] = static_cast<uint8_t>((colorCode << 4) & 0xF0);
    slotType[0] |= dtValue & 0x0F;
    CGolay2087::encode(slotType);
    payload[12] = static_cast<uint8_t>((payload[12] & 0xC0) | ((slotType[0] >> 2) & 0x3F));
    payload[13] = static_cast<uint8_t>((payload[13] & 0x0F) | ((slotType[0] << 6) & 0xC0) | ((slotType[1] >> 2) & 0x30));
    payload[19] = static_cast<uint8_t>((payload[19] & 0xF0) | ((slotType[1] >> 2) & 0x0F));
    payload[20] = static_cast<uint8_t>((payload[20] & 0x03) | ((slotType[1] << 6) & 0xC0) | ((slotType[2] >> 2) & 0x3C));

    CBPTC19696 bptc;
    bptc.encode(lc, payload.data());
    return payload;
}

// Ported from ReplaceEMBInBuffer -- fills the sync/embedded-signaling gap
// (payload offsets 13-19, i.e. packet offsets 33-39) in a voice frame's
// payload: a fixed sync pattern for frameInBurst 0, an embedded LC
// fragment (Golay/QR-protected) for frameInBurst 1-4, or a null EMB
// (still QR-protected, just no LC content) for frameInBurst 5.
void writeSyncOrEmb(std::array<uint8_t, 33> &payload, int frameInBurst, const EmbeddedLC &embeddedLC,
                     unsigned colorCode) {
    if (frameInBurst == 0) {
        payload[13] = static_cast<uint8_t>(payload[13] | (kDmrSyncMSVoice[0] & 0x0F));
        std::memcpy(&payload[14], kDmrSyncMSVoice + 1, 5);
        payload[19] = static_cast<uint8_t>(payload[19] | (kDmrSyncMSVoice[6] & 0xF0));
        return;
    }

    if (frameInBurst >= 1 && frameInBurst <= 4) {
        uint8_t lcss = (frameInBurst == 1) ? 1 : (frameInBurst == 4) ? 2 : 3;
        const uint8_t *fragment = embeddedLC.data() + (frameInBurst - 1) * 4;

        uint8_t emb[2];
        emb[0] = static_cast<uint8_t>((colorCode << 4) & 0xF0);
        emb[0] = static_cast<uint8_t>(emb[0] | ((lcss << 1) & 0x06));
        emb[1] = 0x00;
        CQR1676::encode(emb);

        payload[13] = static_cast<uint8_t>((payload[13] & 0xF0) | ((emb[0] >> 4) & 0x0F));
        payload[14] = static_cast<uint8_t>(((emb[0] << 4) & 0xF0) | ((fragment[0] >> 4) & 0x0F));
        payload[15] = static_cast<uint8_t>(((fragment[0] << 4) & 0xF0) | ((fragment[1] >> 4) & 0x0F));
        payload[16] = static_cast<uint8_t>(((fragment[1] << 4) & 0xF0) | ((fragment[2] >> 4) & 0x0F));
        payload[17] = static_cast<uint8_t>(((fragment[2] << 4) & 0xF0) | ((fragment[3] >> 4) & 0x0F));
        payload[18] = static_cast<uint8_t>(((fragment[3] << 4) & 0xF0) | ((emb[1] >> 4) & 0x0F));
        payload[19] = static_cast<uint8_t>(((emb[1] << 4) & 0xF0) | (payload[19] & 0x0F));
        return;
    }

    // frameInBurst == 5: null EMB, no LC fragment.
    uint8_t emb[2];
    emb[0] = static_cast<uint8_t>((colorCode << 4) & 0xF0);
    emb[1] = 0x00;
    CQR1676::encode(emb);
    payload[13] = static_cast<uint8_t>((payload[13] & 0xF0) | ((emb[0] >> 4) & 0x0F));
    payload[14] = static_cast<uint8_t>((emb[0] << 4) & 0xF0);
    payload[15] = 0;
    payload[16] = 0;
    payload[17] = 0;
    payload[18] = static_cast<uint8_t>((emb[1] >> 4) & 0x0F);
    payload[19] = static_cast<uint8_t>(((emb[1] << 4) & 0xF0) | (payload[19] & 0x0F));
}

} // namespace

EmbeddedLC encodeEmbeddedLC(uint32_t srcId, uint32_t dstId, CallType callType) {
    // Ported from EncodeEmbeddedLC: builds the 9-byte LC (dstId, srcId), a
    // 5-bit CRC over it, Hamming(16,11,4)-protects it across seven 16-bit
    // groups, then interleaves those 128 bits by a fixed period-16 stride
    // into the 16-byte result that gets sliced into quarters by
    // writeSyncOrEmb() above.
    uint8_t lc[9] = {};
    lc[0] = callType == CallType::Private ? 3 : 0; // FLCO -- see buildLcPayload's identical comment
    lc[3] = static_cast<uint8_t>(dstId >> 16);
    lc[4] = static_cast<uint8_t>(dstId >> 8);
    lc[5] = static_cast<uint8_t>(dstId);
    lc[6] = static_cast<uint8_t>(srcId >> 16);
    lc[7] = static_cast<uint8_t>(srcId >> 8);
    lc[8] = static_cast<uint8_t>(srcId);

    bool lcBits[72];
    for (int i = 0; i < 9; i++) CUtils::byteToBitsBE(lc[i], lcBits + i * 8);

    unsigned int crc = 0;
    CCRC::encodeFiveBit(lcBits, crc);

    bool data[128] = {};
    data[106] = (crc & 0x01) == 0x01;
    data[90] = (crc & 0x02) == 0x02;
    data[74] = (crc & 0x04) == 0x04;
    data[58] = (crc & 0x08) == 0x08;
    data[42] = (crc & 0x10) == 0x10;

    unsigned int b = 0;
    for (unsigned int a = 0; a < 11; a++, b++) data[a] = lcBits[b];
    for (unsigned int a = 16; a < 27; a++, b++) data[a] = lcBits[b];
    for (unsigned int a = 32; a < 42; a++, b++) data[a] = lcBits[b];
    for (unsigned int a = 48; a < 58; a++, b++) data[a] = lcBits[b];
    for (unsigned int a = 64; a < 74; a++, b++) data[a] = lcBits[b];
    for (unsigned int a = 80; a < 90; a++, b++) data[a] = lcBits[b];
    for (unsigned int a = 96; a < 106; a++, b++) data[a] = lcBits[b];

    for (unsigned int a = 0; a < 112; a += 16) CHamming::encode16114(data + a);

    for (unsigned int a = 0; a < 16; a++)
        data[a + 112] = data[a] ^ data[a + 16] ^ data[a + 32] ^ data[a + 48] ^ data[a + 64] ^ data[a + 80] ^ data[a + 96];

    bool raw[128];
    b = 0;
    for (unsigned int a = 0; a < 128; a++) {
        raw[a] = data[b];
        b += 16;
        if (b > 127) b -= 127;
    }

    EmbeddedLC result{};
    for (int a = 0; a < 16; a++) CUtils::bitsToByteBE(raw + a * 8, result[a]);
    return result;
}

namespace {
uint8_t slotAndCallBits(const TxParams &params) {
    return static_cast<uint8_t>((params.timeSlot == TimeSlot::Slot2 ? DMR_SLOT2_BIT : 0) |
                                 (params.callType == CallType::Private ? DMR_PRIVATE_CALL_BIT : 0));
}
} // namespace

std::vector<uint8_t> buildHeaderFrame(uint32_t srcId, uint32_t dstId, uint32_t rptrId, uint32_t streamId,
                                       uint8_t seqId, const TxParams &params) {
    // DATASYNC<<4 | slot | call-type | SLOTTYPE_HEADER
    uint8_t bitField = static_cast<uint8_t>((2 << 4) | slotAndCallBits(params) | 1);
    std::vector<uint8_t> pkt = buildPrologue(srcId, dstId, rptrId, seqId, bitField, streamId);

    auto payload = buildLcPayload(srcId, dstId, DMR_DT_VOICE_LC_HEADER, DMR_VOICE_LC_HEADER_CRC_MASK, params.callType,
                                   params.colorCode);
    pkt.insert(pkt.end(), payload.begin(), payload.end());
    pkt.push_back(0); // BER
    pkt.push_back(0); // RSSI
    return pkt;
}

std::vector<uint8_t> buildTerminatorFrame(uint32_t srcId, uint32_t dstId, uint32_t rptrId, uint32_t streamId,
                                           uint8_t seqId, const TxParams &params) {
    // DATASYNC<<4 | slot | call-type | SLOTTYPE_TERMINATOR
    uint8_t bitField = static_cast<uint8_t>((2 << 4) | slotAndCallBits(params) | 2);
    std::vector<uint8_t> pkt = buildPrologue(srcId, dstId, rptrId, seqId, bitField, streamId);

    auto payload = buildLcPayload(srcId, dstId, DMR_DT_TERMINATOR_WITH_LC, DMR_TERMINATOR_WITH_LC_CRC_MASK,
                                   params.callType, params.colorCode);
    pkt.insert(pkt.end(), payload.begin(), payload.end());
    pkt.push_back(0);
    pkt.push_back(0);
    return pkt;
}

std::vector<uint8_t> buildVoiceFrame(uint32_t srcId, uint32_t dstId, uint32_t rptrId, uint32_t streamId,
                                      uint8_t seqId, int frameInBurst, const uint8_t ambe0[AMBE_FRAME_SIZE],
                                      const uint8_t ambe1[AMBE_FRAME_SIZE], const uint8_t ambe2[AMBE_FRAME_SIZE],
                                      const EmbeddedLC &embeddedLC, const TxParams &params) {
    // FRAMETYPE_VOICESYNC(1) for frame A (the resync point), FRAMETYPE_VOICE(0) otherwise.
    uint8_t bitField = static_cast<uint8_t>(slotAndCallBits(params) | ((frameInBurst == 0 ? 1 : 0) << 4) |
                                             (frameInBurst & 0x0F));
    std::vector<uint8_t> pkt = buildPrologue(srcId, dstId, rptrId, seqId, bitField, streamId);

    std::array<uint8_t, 33> payload{};
    std::memcpy(payload.data() + 0, ambe0, AMBE_FRAME_SIZE);
    // ambe1's middle byte (index 4) straddles the sync/EMB gap -- see the
    // matching comment in extractVoiceFrame() below for why. Both writes
    // here put a full copy of ambe1[4] at each position; only its high
    // nibble survives at payload[13] (masked to 0xF0 next, then the sync/
    // EMB write below fills the now-zero low nibble) and only its low
    // nibble survives at payload[19] (the sync/EMB write below only ORs
    // into that byte's high nibble, leaving the low nibble -- ambe1[4]'s
    // original low bits -- untouched).
    std::memcpy(payload.data() + 9, ambe1, 5);
    payload[13] = static_cast<uint8_t>(payload[13] & 0xF0);
    std::memcpy(payload.data() + 19, ambe1 + 4, 5);
    std::memcpy(payload.data() + 24, ambe2, AMBE_FRAME_SIZE);

    writeSyncOrEmb(payload, frameInBurst, embeddedLC, params.colorCode);

    pkt.insert(pkt.end(), payload.begin(), payload.end());
    pkt.push_back(0); // BER
    pkt.push_back(0); // RSSI
    return pkt;
}

bool extractVoiceFrame(const uint8_t *packet, size_t length, uint8_t ambe0[AMBE_FRAME_SIZE],
                        uint8_t ambe1[AMBE_FRAME_SIZE], uint8_t ambe2[AMBE_FRAME_SIZE]) {
    if (length != DMRD_PACKET_SIZE || std::memcmp(packet, "DMRD", 4) != 0) return false;

    uint8_t frameType = (packet[15] & 0x30) >> 4;
    bool slot2 = (packet[15] & 0x80) != 0;
    // Call type (packet[15] & 0x40) deliberately not checked -- a Private
    // call's voice frames need decoding same as a Group call's (e.g.
    // BrandMeister's Parrot echo test replies via Private call), and the
    // caller already has srcId/dstId from the packet if it cares which.
    if (!slot2) return false;
    if (frameType != 0 /* VOICE */ && frameType != 1 /* VOICESYNC */) return false;

    const uint8_t *payload = packet + 20;
    std::memcpy(ambe0, payload + 0, AMBE_FRAME_SIZE);
    // ambe1 straddles the sync/EMB gap at the bit level (108 voice bits,
    // then 48 sync/EMB bits, then 108 more voice bits -- not a clean byte
    // boundary), so its middle byte (index 4) isn't stored as a whole byte
    // on the wire: the encode side writes its high nibble into payload[13]
    // (immediately followed by sync/EMB data in that same byte's low
    // nibble) and its low nibble into payload[19] (preceded by sync/EMB
    // data in that byte's high nibble). Ported from xlxd's own
    // IsValidDvFramePacket: dmr3ambe[13] = (dmrframe[13]&0xF0) |
    // (dmrframe[19]&0x0F), which is why this can't be a plain memcpy the
    // way the rest of this function is.
    std::memcpy(ambe1, payload + 9, 4);
    ambe1[4] = static_cast<uint8_t>((payload[13] & 0xF0) | (payload[19] & 0x0F));
    std::memcpy(ambe1 + 5, payload + 20, 4);
    std::memcpy(ambe2, payload + 24, AMBE_FRAME_SIZE);
    return true;
}

} // namespace dmr
