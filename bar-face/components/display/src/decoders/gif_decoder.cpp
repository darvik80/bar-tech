/// Minimal GIF87a/89a decoder — memory-buffer backed.
/// Supports animated GIFs, disposal methods 0-3, transparency, interlace.

#include "gif_decoder.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include <cstdlib>
#include <cstring>
#include <algorithm>

static constexpr char TAG_GIF[] = "GIFdec";

// ── LZW constants ──────────────────────────────────────────────────────

static constexpr int kLzwMaxCode  = 4096;

// ── Internal handle ────────────────────────────────────────────────────

namespace embed::display {

struct GifHandle {
    // Source buffer (not owned)
    const uint8_t* data;
    size_t         size;
    size_t         pos;

    // Logical screen
    GifInfo  info;
    uint8_t  bg_color;

    // Global color table (up to 256 entries * 3 bytes)
    uint8_t  gct[256 * 3];

    // Local color table for current frame
    uint8_t  lct[256 * 3];
    bool     has_lct;

    // Canvas for compositing (canvas_w * canvas_h * 3)
    uint8_t* canvas;
    // Previous canvas for disposal method 3
    uint8_t* canvas_prev;

    // Current frame
    GifFrame frame;
    int      transparent_idx;   // -1 = no transparency

    // LZW decode buffer (one full sub-image: w*h indices)
    uint8_t* lzw_out;

    // Frame start position for rewind
    size_t   first_frame_pos;

    // LZW decoder state (~36 KB: prefix/suffix/length tables).
    // Held in the heap via heap_caps_malloc(MALLOC_CAP_SPIRAM) rather
    // than on the stack — the animation task has only a few KB of
    // stack, and a 36 KB LzwState on the stack overflows into heap,
    // FreeRTOS internals and other tasks' memory.
    void*    lzw_state = nullptr;
};

// ── Read helpers ───────────────────────────────────────────────────────

static uint8_t  g_byte(GifHandle* g) {
    return (g->pos < g->size) ? g->data[g->pos++] : 0;
}
static uint16_t g_word(GifHandle* g) {
    uint8_t lo = g_byte(g);
    uint8_t hi = g_byte(g);
    return uint16_t(lo) | (uint16_t(hi) << 8);
}
static void g_skip(GifHandle* g, size_t n) {
    if (g->pos + n <= g->size) g->pos += n;
    else g->pos = g->size;
}
static void g_read(GifHandle* g, void* dst, size_t n) {
    size_t avail = (g->pos + n <= g->size) ? n : (g->size - g->pos);
    if (avail > 0) std::memcpy(dst, g->data + g->pos, avail);
    g->pos += n;
}

// Skip sub-blocks (sequence of: size-byte, data...; terminated by 0)
static void skip_sub_blocks(GifHandle* g) {
    while (true) {
        uint8_t sz = g_byte(g);
        if (sz == 0) break;
        g_skip(g, sz);
    }
}

// ── LZW decoder ────────────────────────────────────────────────────────

struct LzwState {
    const uint8_t* src;       // sub-block data source
    size_t         src_len;
    size_t         src_pos;

    int  min_code_size;
    int  clear_code;
    int  eoi_code;
    int  code_size;
    int  next_code;
    int  max_code;

    // Bit reader
    uint32_t bit_buf;
    int      bit_count;

    // String table
    int      prefix[kLzwMaxCode];
    uint8_t  suffix[kLzwMaxCode];
    int      length[kLzwMaxCode];

    // Scratch buffer for string reversal in lzw_decode. Lives in the
    // heap (with the rest of LzwState) so the per-frame stack budget
    // stays small — the original local `uint8_t tmp[4096]` inside
    // lzw_decode's hot loop was enough to push the task past its 4 KB
    // stack on top of the LzwState.
    uint8_t  tmp[4096];
};

static void lzw_init(LzwState* s, const uint8_t* src, size_t len,
                       int min_code_size)
{
    s->src = src;
    s->src_len = len;
    s->src_pos = 0;
    s->min_code_size = min_code_size;
    s->clear_code = 1 << min_code_size;
    s->eoi_code   = s->clear_code + 1;
    s->bit_buf    = 0;
    s->bit_count  = 0;
    s->code_size  = min_code_size + 1;
    s->next_code  = s->clear_code + 2;
    s->max_code   = 1 << s->code_size;
}

static int lzw_read_bits(LzwState* s, int nbits) {
    while (s->bit_count < nbits) {
        if (s->src_pos >= s->src_len) return -1;
        s->bit_buf |= uint32_t(s->src[s->src_pos++]) << s->bit_count;
        s->bit_count += 8;
    }
    int val = int(s->bit_buf) & ((1 << nbits) - 1);
    s->bit_buf >>= nbits;
    s->bit_count -= nbits;
    return val;
}

static void lzw_reset_table(LzwState* s) {
    s->code_size = s->min_code_size + 1;
    s->next_code = s->clear_code + 2;
    s->max_code  = 1 << s->code_size;
    // Pre-fill base entries
    for (int i = 0; i < s->clear_code; ++i) {
        s->prefix[i] = -1;
        s->suffix[i] = static_cast<uint8_t>(i);
        s->length[i] = 1;
    }
}

/// Decode LZW data into output buffer. Returns number of pixels decoded.
static int lzw_decode(LzwState* s, uint8_t* out, int max_pixels)
{
    lzw_reset_table(s);

    int out_pos = 0;
    int prev_code = -1;

    while (out_pos < max_pixels) {
        int code = lzw_read_bits(s, s->code_size);
        if (code < 0 || code == s->eoi_code) break;

        if (code == s->clear_code) {
            lzw_reset_table(s);
            prev_code = -1;
            continue;
        }

        if (prev_code == -1) {
            // First code after clear — must be a base code
            if (code < s->clear_code) {
                out[out_pos++] = static_cast<uint8_t>(code);
                prev_code = code;
            }
            continue;
        }

        // Output the string for this code
        // First, figure out what string to emit
        int emit_code = code;
        uint8_t first_byte = 0;

        if (code < s->next_code) {
            // Code exists in table — output string, then add new entry
            // Walk the string backwards to find first byte
            int c = code;
            while (s->prefix[c] >= 0) c = s->prefix[c];
            first_byte = s->suffix[c];
        } else {
            // KwKwK case: code == next_code
            // The string is prev_string + first_byte_of_prev_string
            int c = prev_code;
            while (s->prefix[c] >= 0) c = s->prefix[c];
            first_byte = s->suffix[c];
            emit_code = prev_code; // emit prev string, then append first_byte
        }

        // Write the string for emit_code (in reverse, then first_byte if KwKwK)
        int len = s->length[emit_code];
        int total = (code >= s->next_code) ? len + 1 : len;

        if (out_pos + total > max_pixels) total = max_pixels - out_pos;

        // Build in temp, then copy. Buffer lives in LzwState (heap)
        // so the per-frame stack budget stays small.
        int tlen = 0;
        {
            int c = emit_code;
            while (c >= 0 && tlen < len && tlen < int(sizeof(s->tmp))) {
                s->tmp[tlen++] = s->suffix[c];
                c = s->prefix[c];
            }
        }
        // Reverse into output
        for (int i = tlen - 1; i >= 0 && out_pos < max_pixels; --i) {
            out[out_pos++] = s->tmp[i];
        }
        if (code >= s->next_code && out_pos < max_pixels) {
            out[out_pos++] = first_byte;
        }

        // Add new table entry: prev_string + first_byte
        if (s->next_code < kLzwMaxCode) {
            s->prefix[s->next_code] = prev_code;
            s->suffix[s->next_code] = first_byte;
            s->length[s->next_code] = s->length[prev_code] + 1;
            s->next_code++;
            if (s->next_code > s->max_code && s->code_size < 12) {
                s->code_size++;
                s->max_code = 1 << s->code_size;
            }
        }

        prev_code = code;
    }

    return out_pos;
}

// ── Collect sub-blocks into contiguous buffer ──────────────────────────

/// Read GIF sub-blocks starting at current position into dst.
/// Returns total bytes written. Advances g->pos past the sub-block stream.
static size_t collect_sub_blocks(GifHandle* g, uint8_t* dst, size_t dst_cap)
{
    size_t total = 0;
    while (true) {
        uint8_t sz = g_byte(g);
        if (sz == 0) break;
        size_t copy = std::min(size_t(sz), dst_cap - total);
        if (copy > 0) {
            g_read(g, dst + total, copy);
            total += copy;
        }
        if (size_t(sz) > copy) g_skip(g, sz - copy); // skip overflow
    }
    return total;
}

// ── Deinterlace ────────────────────────────────────────────────────────

static void deinterlace(uint8_t* pixels, int w, int h) {
    // GIF interlace: pass 1: rows 0,8,16..  pass 2: 4,12,20..
    // pass 3: 2,6,10..  pass 4: 1,3,5..
    uint8_t* tmp = static_cast<uint8_t*>(malloc(w * h));
    if (!tmp) return;
    std::memcpy(tmp, pixels, w * h);

    static const int starts[] = {0, 4, 2, 1};
    static const int steps[]  = {8, 8, 4, 2};
    int src_row = 0;
    for (int pass = 0; pass < 4; ++pass) {
        for (int y = starts[pass]; y < h; y += steps[pass]) {
            if (src_row < h)
                std::memcpy(pixels + y * w, tmp + src_row * w, w);
            src_row++;
        }
    }
    free(tmp);
}

// ── Open ───────────────────────────────────────────────────────────────

GifHandle* gif_open(const uint8_t* data, size_t size)
{
    if (size < 13) return nullptr;

    auto* g = new GifHandle{};
    g->data = data;
    g->size = size;
    g->pos  = 0;
    g->transparent_idx = -1;

    // Signature "GIF87a" or "GIF89a"
    char sig[7] = {};
    g_read(g, sig, 6);
    if (std::memcmp(sig, "GIF87a", 6) != 0 &&
        std::memcmp(sig, "GIF89a", 6) != 0) {
        ESP_LOGE(TAG_GIF, "invalid GIF signature");
        delete g;
        return nullptr;
    }

    // Logical Screen Descriptor
    g->info.canvas_w = g_word(g);
    g->info.canvas_h = g_word(g);
    uint8_t packed = g_byte(g);
    g->bg_color = g_byte(g);
    g_byte(g); // pixel aspect ratio

    g->info.has_gct = (packed >> 7) & 1;
    int gct_size_bits = packed & 0x07;

    if (g->info.canvas_w == 0 || g->info.canvas_h == 0 ||
        g->info.canvas_w > 2048 || g->info.canvas_h > 2048) {
        ESP_LOGE(TAG_GIF, "invalid canvas %ux%u", g->info.canvas_w, g->info.canvas_h);
        delete g;
        return nullptr;
    }

    // Global Color Table
    if (g->info.has_gct) {
        g->info.gct_entries = 1 << (gct_size_bits + 1);
        g_read(g, g->gct, g->info.gct_entries * 3);
    }

    // Allocate canvas (PSRAM)
    size_t canvas_bytes = size_t(g->info.canvas_w) * g->info.canvas_h * 3;
    g->canvas = static_cast<uint8_t*>(
        heap_caps_malloc(canvas_bytes, MALLOC_CAP_SPIRAM));
    g->canvas_prev = static_cast<uint8_t*>(
        heap_caps_malloc(canvas_bytes, MALLOC_CAP_SPIRAM));
    if (!g->canvas || !g->canvas_prev) {
        ESP_LOGE(TAG_GIF, "canvas alloc failed (%u bytes)", (unsigned)canvas_bytes);
        heap_caps_free(g->canvas);
        heap_caps_free(g->canvas_prev);
        delete g;
        return nullptr;
    }

    // Fill canvas with background color
    uint8_t bg_r = 0, bg_g = 0, bg_b = 0;
    if (g->info.has_gct && g->bg_color < g->info.gct_entries) {
        bg_r = g->gct[g->bg_color * 3 + 0];
        bg_g = g->gct[g->bg_color * 3 + 1];
        bg_b = g->gct[g->bg_color * 3 + 2];
    }
    for (size_t i = 0; i < size_t(g->info.canvas_w) * g->info.canvas_h; ++i) {
        g->canvas[i * 3 + 0] = bg_r;
        g->canvas[i * 3 + 1] = bg_g;
        g->canvas[i * 3 + 2] = bg_b;
    }

    // LZW output buffer (max sub-image = canvas size)
    g->lzw_out = static_cast<uint8_t*>(
        heap_caps_malloc(canvas_bytes, MALLOC_CAP_SPIRAM));
    if (!g->lzw_out) {
        ESP_LOGE(TAG_GIF, "LZW buffer alloc failed");
        heap_caps_free(g->canvas);
        heap_caps_free(g->canvas_prev);
        delete g;
        return nullptr;
    }

    // LZW decoder state — see comment on GifHandle::lzw_state.
    g->lzw_state = heap_caps_malloc(sizeof(LzwState), MALLOC_CAP_SPIRAM);
    if (!g->lzw_state) {
        ESP_LOGE(TAG_GIF, "LZW state alloc failed (%u bytes)",
                 (unsigned)sizeof(LzwState));
        heap_caps_free(g->lzw_out);
        heap_caps_free(g->canvas);
        heap_caps_free(g->canvas_prev);
        delete g;
        return nullptr;
    }
    std::memset(g->lzw_state, 0, sizeof(LzwState));

    // Record position for rewind
    g->first_frame_pos = g->pos;

    ESP_LOGI(TAG_GIF, "GIF opened: %ux%u gct=%d bg=%d",
             g->info.canvas_w, g->info.canvas_h,
             g->info.has_gct, g->bg_color);
    return g;
}

// ── Close ──────────────────────────────────────────────────────────────

void gif_close(GifHandle* g) {
    if (!g) return;
    heap_caps_free(g->lzw_state);
    heap_caps_free(g->canvas);
    heap_caps_free(g->canvas_prev);
    heap_caps_free(g->lzw_out);
    delete g;
}

// ── Accessors ──────────────────────────────────────────────────────────

const GifInfo&  gif_info(const GifHandle* g)  { return g->info; }
const GifFrame& gif_frame(const GifHandle* g) { return g->frame; }
const uint8_t*  gif_canvas(const GifHandle* g) { return g->canvas; }

// ── Decode next frame ──────────────────────────────────────────────────

int gif_next_frame(GifHandle* g)
{
    g->transparent_idx = -1;
    g->has_lct = false;
    g->frame.delay_cs = 10; // default 100ms
    g->frame.disposal = 0;

    // Scan blocks until we find an image descriptor (0x2C)
    while (g->pos < g->size) {
        uint8_t sep = g_byte(g);

        if (sep == 0x2C) {
            // ── Image Descriptor ──
            g->frame.x = g_word(g);
            g->frame.y = g_word(g);
            g->frame.w = g_word(g);
            g->frame.h = g_word(g);
            uint8_t packed = g_byte(g);
            g->has_lct = (packed >> 7) & 1;
            g->frame.interlaced = (packed >> 6) & 1;

            if (g->has_lct) {
                int lct_bits = (packed & 0x07) + 1;
                int lct_entries = 1 << lct_bits;
                g_read(g, g->lct, lct_entries * 3);
            }

            // ── LZW decode ──
            uint8_t min_code = g_byte(g);
            if (min_code < 2 || min_code > 11) {
                ESP_LOGE(TAG_GIF, "bad LZW min code size %d", min_code);
                return 0;
            }

            // Collect all sub-blocks into a contiguous buffer
            size_t sub_cap = size_t(g->frame.w) * g->frame.h * 2 + 256;
            uint8_t* sub_buf = static_cast<uint8_t*>(malloc(sub_cap));
            if (!sub_buf) {
                ESP_LOGE(TAG_GIF, "sub-block alloc failed");
                return 0;
            }
            size_t sub_len = collect_sub_blocks(g, sub_buf, sub_cap);

            int pixels = int(g->frame.w) * int(g->frame.h);
            // LzwState lives in g->lzw_state (heap) — see GifHandle.
            auto* lzw = static_cast<LzwState*>(g->lzw_state);
            lzw_init(lzw, sub_buf, sub_len, min_code);
            int decoded = lzw_decode(lzw, g->lzw_out, pixels);
            free(sub_buf);

            if (decoded < pixels) {
                ESP_LOGW(TAG_GIF, "LZW short: decoded %d of %d pixels",
                         decoded, pixels);
            }

            // Deinterlace if needed
            if (g->frame.interlaced) {
                deinterlace(g->lzw_out, g->frame.w, g->frame.h);
            }

            // ── Disposal: save canvas for method 3 ──
            if (g->frame.disposal == 3) {
                std::memcpy(g->canvas_prev, g->canvas,
                            size_t(g->info.canvas_w) * g->info.canvas_h * 3);
            }

            // ── Composite sub-image onto canvas ──
            const uint8_t* ct = g->has_lct ? g->lct : g->gct;
            for (int row = 0; row < g->frame.h; ++row) {
                int cy = g->frame.y + row;
                if (cy >= g->info.canvas_h) break;
                for (int col = 0; col < g->frame.w; ++col) {
                    int cx = g->frame.x + col;
                    if (cx >= g->info.canvas_w) break;

                    int idx = g->lzw_out[row * g->frame.w + col];
                    if (idx == g->transparent_idx) continue;

                    int canvas_off = (cy * g->info.canvas_w + cx) * 3;
                    g->canvas[canvas_off + 0] = ct[idx * 3 + 0];
                    g->canvas[canvas_off + 1] = ct[idx * 3 + 1];
                    g->canvas[canvas_off + 2] = ct[idx * 3 + 2];
                }
            }

            return 1; // frame ready
        }
        else if (sep == 0x21) {
            // ── Extension block ──
            uint8_t label = g_byte(g);

            if (label == 0xF9) {
                // Graphic Control Extension (always 4 bytes + terminator)
                uint8_t block_size = g_byte(g);
                (void)block_size; // should be 4
                uint8_t packed = g_byte(g);
                g->frame.disposal = (packed >> 2) & 0x07;
                g->transparent_idx = (packed & 1) ? int(g_byte(g)) : -1;
                if (!(packed & 1)) g_byte(g); // skip transparent index
                g->frame.delay_cs = g_word(g);
                if (g->frame.delay_cs == 0) g->frame.delay_cs = 10;
                g_byte(g); // block terminator (0)
            } else {
                // Other extensions: skip sub-blocks
                skip_sub_blocks(g);
            }
        }
        else if (sep == 0x3B) {
            // ── Trailer ──
            return 0;
        }
        else {
            // Unknown byte — skip
            continue;
        }
    }

    return 0; // end of data
}

// ── Rewind ─────────────────────────────────────────────────────────────

void gif_rewind(GifHandle* g)
{
    g->pos = g->first_frame_pos;
    g->transparent_idx = -1;

    // Reset canvas to background
    uint8_t bg_r = 0, bg_g = 0, bg_b = 0;
    if (g->info.has_gct && g->bg_color < g->info.gct_entries) {
        bg_r = g->gct[g->bg_color * 3 + 0];
        bg_g = g->gct[g->bg_color * 3 + 1];
        bg_b = g->gct[g->bg_color * 3 + 2];
    }
    for (size_t i = 0; i < size_t(g->info.canvas_w) * g->info.canvas_h; ++i) {
        g->canvas[i * 3 + 0] = bg_r;
        g->canvas[i * 3 + 1] = bg_g;
        g->canvas[i * 3 + 2] = bg_b;
    }
}

} // namespace embed::display
