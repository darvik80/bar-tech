#pragma once

#include <cstdint>
#include <cstddef>

namespace embed::display {

/// Decode PNG from memory buffer to RGBA8888 pixel data.
/// Caller must free(*out) with free().
/// Returns 0 on success, non-zero error code on failure.
int png_decode_rgba(const uint8_t* data, size_t size,
                    uint8_t** out, uint32_t* w, uint32_t* h);

} // namespace embed::display
