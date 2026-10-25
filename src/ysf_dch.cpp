#include "ysf_dch.h"

#include <cstring>

#include "ccrc.h"
#include "cysfconvolution.h"

namespace ysf {

namespace {

// The PN sequence YSF whitens its fields with -- the data only: the CRC is
// computed over the whitened bytes and isn't whitened itself.
const uint8_t WHITENING[] = {0x93, 0xD7, 0x51, 0x21, 0x9C, 0x2F, 0x6C, 0xD0, 0xEF, 0x0F,
                             0xF8, 0x3D, 0xF1, 0x73, 0x20, 0x94, 0xED, 0x1E, 0x7C, 0xD8};

constexpr unsigned int PAYLOAD_OFFSET = 30; // after the 5-byte sync and 25-byte FICH
constexpr unsigned int BLOCKS = 5;
constexpr unsigned int BLOCK_BYTES = 18;

bool readBit(const uint8_t *p, unsigned int i) { return (p[i >> 3] >> (7 - (i & 7))) & 1; }
void writeBit(uint8_t *p, unsigned int i, bool b) {
    if (b) p[i >> 3] |= 0x80 >> (i & 7);
    else p[i >> 3] &= ~(0x80 >> (i & 7));
}

// One field's layout: where its coded bytes sit in each block, and how many
// columns its interleaver has (the coded bits are read out in pairs, 20 rows
// of `columns` -- the FICH's 5x20 for the 10-byte field).
struct Layout {
    unsigned int chunkOffset; // in each 18-byte block
    unsigned int chunkBytes;
    unsigned int columns;
    unsigned int dataBytes;
};
const Layout VD2_LAYOUT{0, 5, 5, VD2_DATA_BYTES};
const Layout CSD1_LAYOUT{0, 9, 9, CSD_BYTES};
const Layout CSD2_LAYOUT{9, 9, 9, CSD_BYTES};

unsigned int interleaved(unsigned int pair, unsigned int columns) { return (pair % columns) * 40 + (pair / columns) * 2; }

bool decodeField(const uint8_t *frame, const Layout &l, uint8_t *out) {
    uint8_t coded[45] = {};
    for (unsigned int j = 0; j < BLOCKS; j++)
        std::memcpy(coded + j * l.chunkBytes, frame + PAYLOAD_OFFSET + j * BLOCK_BYTES + l.chunkOffset, l.chunkBytes);
    const unsigned int pairs = BLOCKS * l.chunkBytes * 8 / 2; // = data + CRC + 4 tail bits

    CYSFConvolution viterbi;
    viterbi.start();
    for (unsigned int i = 0; i < pairs; i++) {
        unsigned int n = interleaved(i, l.columns);
        viterbi.decode(readBit(coded, n) ? 1 : 0, readBit(coded, n + 1) ? 1 : 0);
    }
    uint8_t decoded[23] = {};
    viterbi.chainback(decoded, pairs - 4); // the data and CRC, without the tail bits (as the FICH does)
    if (!CCRC::checkCCITT162(decoded, l.dataBytes + 2)) return false;
    for (unsigned int i = 0; i < l.dataBytes; i++) out[i] = decoded[i] ^ WHITENING[i];
    return true;
}

void encodeField(const uint8_t *data, const Layout &l, uint8_t *frame) {
    uint8_t plain[23] = {}; // data, CRC, then zero tail bits
    for (unsigned int i = 0; i < l.dataBytes; i++) plain[i] = data[i] ^ WHITENING[i];
    CCRC::addCCITT162(plain, l.dataBytes + 2);
    const unsigned int pairs = BLOCKS * l.chunkBytes * 8 / 2;

    uint8_t convolved[45] = {};
    CYSFConvolution conv;
    conv.encode(plain, convolved, pairs);

    uint8_t coded[45] = {};
    for (unsigned int i = 0; i < pairs; i++) {
        unsigned int n = interleaved(i, l.columns);
        writeBit(coded, n, readBit(convolved, 2 * i));
        writeBit(coded, n + 1, readBit(convolved, 2 * i + 1));
    }
    for (unsigned int j = 0; j < BLOCKS; j++)
        std::memcpy(frame + PAYLOAD_OFFSET + j * BLOCK_BYTES + l.chunkOffset, coded + j * l.chunkBytes, l.chunkBytes);
}

} // namespace

bool decodeVD2Data(const uint8_t *frame, uint8_t data[VD2_DATA_BYTES]) { return decodeField(frame, VD2_LAYOUT, data); }
void encodeVD2Data(const uint8_t data[VD2_DATA_BYTES], uint8_t *frame) { encodeField(data, VD2_LAYOUT, frame); }

bool decodeHeaderData(const uint8_t *frame, uint8_t csd1[CSD_BYTES], uint8_t csd2[CSD_BYTES]) {
    bool ok1 = decodeField(frame, CSD1_LAYOUT, csd1);
    bool ok2 = decodeField(frame, CSD2_LAYOUT, csd2);
    return ok1 && ok2;
}

void encodeHeaderData(const uint8_t csd1[CSD_BYTES], const uint8_t csd2[CSD_BYTES], uint8_t *frame) {
    encodeField(csd1, CSD1_LAYOUT, frame);
    encodeField(csd2, CSD2_LAYOUT, frame);
}

} // namespace ysf
