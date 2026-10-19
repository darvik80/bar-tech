#pragma once

/// Minimal zlib/deflate decompressor (inflate only).
/// Implements RFC 1951 (DEFLATE) with zlib wrapper (RFC 1950).
/// Sufficient for PNG IDAT decompression on embedded targets.

#include <cstdint>
#include <cstddef>

namespace embed::display {

/// Decompress zlib-wrapped deflate data.
/// @param src     compressed input (with zlib header)
/// @param src_len input size
/// @param dst     output buffer (caller-allocated)
/// @param dst_len [in] capacity, [out] bytes written
/// Returns 0 on success, negative on error.
int inflate_zlib(const uint8_t* src, size_t src_len,
                 uint8_t* dst, size_t* dst_len);

} // namespace embed::display
