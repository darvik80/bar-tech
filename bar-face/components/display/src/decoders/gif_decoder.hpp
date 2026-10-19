#pragma once

/// Minimal GIF87a/89a decoder operating on an in-memory buffer.
/// Supports animated GIFs with disposal methods 0-3 and transparency.

#include <cstdint>
#include <cstddef>

namespace embed::display {

/// Opaque GIF handle.
struct GifHandle;

/// Logical screen descriptor.
struct GifInfo {
    uint16_t canvas_w;
    uint16_t canvas_h;
    bool     has_gct;       ///< global color table present
    uint16_t gct_entries;   ///< number of GCT entries
};

/// Per-frame metadata (populated after gif_next_frame).
struct GifFrame {
    uint16_t x, y;          ///< sub-image position
    uint16_t w, h;          ///< sub-image dimensions
    uint16_t delay_cs;      ///< delay in centiseconds (1/100 s)
    uint8_t  disposal;      ///< 0=none, 1=keep, 2=bg, 3=prev
    bool     interlaced;
};

/// Open a GIF from a memory buffer.  The buffer must remain valid
/// for the lifetime of the returned handle.
/// Returns nullptr on parse error.
GifHandle* gif_open(const uint8_t* data, size_t size);

/// Close and free the handle.
void gif_close(GifHandle* g);

/// Read-only access to logical screen info.
const GifInfo& gif_info(const GifHandle* g);

/// Read-only access to the current frame metadata.
const GifFrame& gif_frame(const GifHandle* g);

/// Pointer to the composited canvas (canvas_w * canvas_h * 3 bytes, RGB888).
/// Updated after each call to gif_next_frame().
const uint8_t* gif_canvas(const GifHandle* g);

/// Decode the next frame and composite onto the canvas.
/// Returns 1 on success, 0 when no more frames (caller may rewind).
int gif_next_frame(GifHandle* g);

/// Rewind to the first frame so the animation can loop.
/// Clears the canvas to the background color.
void gif_rewind(GifHandle* g);

} // namespace embed::display
