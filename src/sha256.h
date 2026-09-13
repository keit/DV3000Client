#pragma once

// Minimal, self-contained SHA-256 (FIPS 180-4) -- the DMR Homebrew/MMDVM
// protocol's RPTK authentication step needs SHA256(salt ++ password), and
// nothing else in this project currently links a crypto library. Vendoring
// this one well-known algorithm keeps that dependency-free, consistent with
// how third_party/serialDV and third_party/xlxd are already vendored rather
// than pulled in as system packages.

#include <cstddef>
#include <cstdint>
#include <string>

namespace dmr {

// Hashes `data` (length bytes) and writes the 32-byte digest to `digest`.
void sha256(const uint8_t *data, size_t length, uint8_t digest[32]);

// Convenience wrapper for the RPTK case: hashes saltBytes (exactly 4 bytes)
// followed by the raw bytes of password, matching the protocol's
// SHA256(salt ++ password) formula.
void sha256(const uint8_t saltBytes[4], const std::string &password, uint8_t digest[32]);

} // namespace dmr
