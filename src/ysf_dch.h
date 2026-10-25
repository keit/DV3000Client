#pragma once

// YSF data channels: the callsigns and other text a Fusion frame carries
// alongside (or instead of) voice. Our own implementation -- the reference
// one (G4KLX's YSFPayload, also in xlxd as cysfpayload) is GPLv2-only, so
// it can't be combined with this GPLv3 code. The building blocks come from
// the GPLv2+ files we do use: CYSFConvolution, CCRC.
//
// Each field is coded the way the FICH is, plus whitening: the data XORed
// with a fixed sequence, then a CCITT CRC-16 over the whitened bytes,
// convolutionally coded (rate 1/2, 4 tail bits) and interleaved column-wise
// in bit pairs. (Checked against real Fusion traffic, where every frame's
// fields pass the CRC this way and read as callsigns.) The coded bits sit
// in the payload's 5 blocks of 18 bytes:
// - V/D mode 2 (DN) communications frames: a 10-byte field, its 200 coded
//   bits in the first 5 bytes of each block (the 13 after are voice);
// - header and terminator frames: two 20-byte fields (CSD1, CSD2), 360
//   coded bits each, in the first and the second 9 bytes of each block.
// Which 10 bytes a DN frame carries depends on its frame number (FICH FN):
// 0 destination callsign, 1 source, 2 downlink, 3 uplink, then radio ID and
// other data. CSD1 is the destination and source callsigns (10 + 10), CSD2
// the downlink and uplink.

#include <cstdint>

namespace ysf {

constexpr unsigned int VD2_DATA_BYTES = 10;
constexpr unsigned int CSD_BYTES = 20;

// frame: a whole 120-byte YSF frame (sync, FICH, payload). Decoders return
// false if the field's CRC doesn't match.
bool decodeVD2Data(const uint8_t *frame, uint8_t data[VD2_DATA_BYTES]);
void encodeVD2Data(const uint8_t data[VD2_DATA_BYTES], uint8_t *frame);

bool decodeHeaderData(const uint8_t *frame, uint8_t csd1[CSD_BYTES], uint8_t csd2[CSD_BYTES]);
void encodeHeaderData(const uint8_t csd1[CSD_BYTES], const uint8_t csd2[CSD_BYTES], uint8_t *frame);

} // namespace ysf
