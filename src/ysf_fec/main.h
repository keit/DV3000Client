// Stand-in for xlxd's main.h, which cysfutils.cpp includes only for the
// integer type names below -- the real one pulls in the whole of xlxd.
// Copied next to the YSF sources in the build tree (see CMakeLists.txt),
// so their same-directory #include "main.h" finds this one.
#pragma once

#include <cstdint>

typedef unsigned char uint8;
typedef unsigned short uint16;
typedef unsigned int uint32;
