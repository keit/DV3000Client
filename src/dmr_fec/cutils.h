#pragma once

// Bit/byte helpers the vendored xlxd FEC encoders (cbptc19696, ccrc) and
// dmr_voice.cpp call as CUtils::. Our own implementation, replacing
// xlxd's cutils.h/.cpp: those carry a GPLv2-only notice, which can't be
// combined with the GPLv3 code in this binary. CMakeLists.txt copies the
// FEC sources next to this file so their #include "cutils.h" lands here.

class CUtils {
public:
    // bits[0] = most significant bit of byte.
    static void byteToBitsBE(unsigned char byte, bool *bits);
    static void bitsToByteBE(const bool *bits, unsigned char &byte);
};
