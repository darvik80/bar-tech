/// Minimal PNG decoder for embedded use — decodes to RGBA8888.
/// Supports 8-bit color types: grayscale, RGB, palette, gray+alpha, RGBA.
/// Uses ESP-IDF's built-in zlib for IDAT decompression.

#include "png_decoder.hpp"
#include "tiny_inflate.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include <cstdlib>
#include <cstring>
#include <algorithm>

static constexpr char TAG_PNG[] = "PNGdec";

// ── PNG signature ──────────────────────────────────────────────────────

static const uint8_t kPngSig[8] = {137, 80, 78, 71, 13, 10, 26, 10};

// ── Byte-order helpers (PNG = big-endian) ──────────────────────────────

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
}

static uint16_t rd16(const uint8_t* p) {
    return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}

// ── IHDR info ──────────────────────────────────────────────────────────

struct PngInfo {
    uint32_t width, height;
    uint8_t  bit_depth;
    uint8_t  color_type;   // 0=gray, 2=RGB, 3=palette, 4=grayA, 6=RGBA
    uint8_t  compression;
    uint8_t  filter;
    uint8_t  interlace;
};

// ── CRC32 (for chunk parsing — simplified, skips validation for speed) ─

// ── Scanline un-filter ─────────────────────────────────────────────────

static void paeth_predictor(uint8_t& out, uint8_t a, uint8_t b, uint8_t c) {
    int p  = int(a) + int(b) - int(c);
    int pa = std::abs(p - int(a));
    int pb = std::abs(p - int(b));
    int pc = std::abs(p - int(c));
    uint8_t pr = (pa <= pb && pa <= pc) ? a : (pb <= pc) ? b : c;
    out = static_cast<uint8_t>(out + pr);
}

static bool unfilter_scanline(uint8_t* cur, const uint8_t* prev,
                               int bpp, int width)
{
    uint8_t ftype = cur[0];
    uint8_t* px = cur + 1;          // pixel data starts after filter byte
    const int row_bytes = width * bpp;

    switch (ftype) {
    case 0: // None
        break;
    case 1: // Sub
        for (int i = bpp; i < row_bytes; ++i)
            px[i] = static_cast<uint8_t>(px[i] + px[i - bpp]);
        break;
    case 2: // Up
        if (prev)
            for (int i = 0; i < row_bytes; ++i)
                px[i] = static_cast<uint8_t>(px[i] + prev[i]);
        break;
    case 3: // Average
        for (int i = 0; i < row_bytes; ++i) {
            uint8_t a = (i >= bpp) ? px[i - bpp] : 0;
            uint8_t b = prev ? prev[i] : 0;
            px[i] = static_cast<uint8_t>(px[i] + ((int(a) + int(b)) / 2));
        }
        break;
    case 4: // Paeth
        for (int i = 0; i < row_bytes; ++i) {
            uint8_t a = (i >= bpp) ? px[i - bpp] : 0;
            uint8_t b = prev ? prev[i] : 0;
            uint8_t c = (prev && i >= bpp) ? prev[i - bpp] : 0;
            paeth_predictor(px[i], a, b, c);
        }
        break;
    default:
        ESP_LOGE(TAG_PNG, "unknown filter type %d", ftype);
        return false;
    }
    return true;
}

// ── Pixel conversion → RGBA8888 ────────────────────────────────────────

static void convert_to_rgba(const uint8_t* src, uint8_t* dst,
                              int width, uint8_t color_type,
                              const uint8_t* palette, int plte_entries,
                              const uint8_t* trns, int trns_len)
{
    switch (color_type) {
    case 0: // Grayscale
        for (int x = 0; x < width; ++x) {
            dst[x * 4 + 0] = src[x];
            dst[x * 4 + 1] = src[x];
            dst[x * 4 + 2] = src[x];
            dst[x * 4 + 3] = (trns && trns_len >= 2 &&
                              src[x] == rd16(trns)) ? 0 : 255;
        }
        break;
    case 2: // RGB
        for (int x = 0; x < width; ++x) {
            dst[x * 4 + 0] = src[x * 3 + 0];
            dst[x * 4 + 1] = src[x * 3 + 1];
            dst[x * 4 + 2] = src[x * 3 + 2];
            dst[x * 4 + 3] = 255;
            // tRNS for RGB: check exact color match
            if (trns && trns_len >= 6 &&
                src[x*3+0] == trns[1] && src[x*3+1] == trns[3] &&
                src[x*3+2] == trns[5])
                dst[x * 4 + 3] = 0;
        }
        break;
    case 3: // Palette
        for (int x = 0; x < width; ++x) {
            int idx = src[x];
            if (idx < plte_entries) {
                dst[x * 4 + 0] = palette[idx * 3 + 0];
                dst[x * 4 + 1] = palette[idx * 3 + 1];
                dst[x * 4 + 2] = palette[idx * 3 + 2];
            }
            dst[x * 4 + 3] = (trns && idx < trns_len) ? trns[idx] : 255;
        }
        break;
    case 4: // Grayscale + Alpha
        for (int x = 0; x < width; ++x) {
            dst[x * 4 + 0] = src[x * 2 + 0];
            dst[x * 4 + 1] = src[x * 2 + 0];
            dst[x * 4 + 2] = src[x * 2 + 0];
            dst[x * 4 + 3] = src[x * 2 + 1];
        }
        break;
    case 6: // RGBA — direct copy
        std::memcpy(dst, src, width * 4);
        break;
    }
}

// ── Main decode entry point ────────────────────────────────────────────

namespace embed::display {

int png_decode_rgba(const uint8_t* data, size_t size,
                    uint8_t** out, uint32_t* w, uint32_t* h)
{
    *out = nullptr;
    *w = *h = 0;

    // 1. Verify PNG signature
    if (size < 8 || std::memcmp(data, kPngSig, 8) != 0) {
        ESP_LOGE(TAG_PNG, "invalid PNG signature");
        return -1;
    }

    // 2. Walk chunks: collect IHDR, PLTE, tRNS, IDAT
    PngInfo info{};
    const uint8_t* palette = nullptr;
    int plte_entries = 0;
    const uint8_t* trns = nullptr;
    int trns_len = 0;

    // IDAT collection (store pointers + lengths, concatenate later)
    static constexpr int kMaxIdat = 64;
    struct IdatRef { const uint8_t* ptr; uint32_t len; };
    IdatRef idats[kMaxIdat];
    int idat_count = 0;
    uint32_t idat_total = 0;

    size_t pos = 8;
    bool have_ihdr = false;

    while (pos + 12 <= size) {
        uint32_t chunk_len = rd32(data + pos);
        uint32_t chunk_type = rd32(data + pos + 4);
        const uint8_t* chunk_data = data + pos + 8;

        if (pos + 12 + chunk_len > size) {
            ESP_LOGE(TAG_PNG, "chunk extends past end of data");
            return -2;
        }

        // IHDR
        if (chunk_type == 0x49484452u) { // "IHDR"
            if (chunk_len < 13) return -3;
            info.width      = rd32(chunk_data);
            info.height     = rd32(chunk_data + 4);
            info.bit_depth  = chunk_data[8];
            info.color_type = chunk_data[9];
            info.compression = chunk_data[10];
            info.filter     = chunk_data[11];
            info.interlace  = chunk_data[12];
            have_ihdr = true;
        }
        // PLTE
        else if (chunk_type == 0x504C5445u) { // "PLTE"
            palette = chunk_data;
            plte_entries = static_cast<int>(chunk_len / 3);
        }
        // tRNS
        else if (chunk_type == 0x74524E53u) { // "tRNS"
            trns = chunk_data;
            trns_len = static_cast<int>(chunk_len);
        }
        // IDAT
        else if (chunk_type == 0x49444154u) { // "IDAT"
            if (idat_count < kMaxIdat) {
                idats[idat_count++] = {chunk_data, chunk_len};
                idat_total += chunk_len;
            }
        }
        // IEND
        else if (chunk_type == 0x49454E44u) { // "IEND"
            break;
        }

        pos += 12 + chunk_len; // length(4) + type(4) + data + CRC(4)
    }

    if (!have_ihdr) {
        ESP_LOGE(TAG_PNG, "missing IHDR chunk");
        return -4;
    }

    // Only 8-bit depth supported
    if (info.bit_depth != 8) {
        ESP_LOGE(TAG_PNG, "unsupported bit depth %d (only 8 supported)",
                 info.bit_depth);
        return -5;
    }
    if (info.interlace != 0) {
        ESP_LOGW(TAG_PNG, "interlaced PNG — decoding may be incorrect");
    }
    if (info.width == 0 || info.height == 0 ||
        info.width > 4096 || info.height > 4096) {
        ESP_LOGE(TAG_PNG, "invalid dimensions %ux%u", info.width, info.height);
        return -6;
    }

    ESP_LOGI(TAG_PNG, "PNG %ux%u ct=%d idat=%u (%u bytes)",
             info.width, info.height, info.color_type,
             idat_count, idat_total);

    // 3. Concatenate IDAT data
    // Prefer SPIRAM: a photo-sized PNG can easily exceed free internal RAM.
    uint8_t* idat_buf = static_cast<uint8_t*>(
        heap_caps_malloc(idat_total, MALLOC_CAP_SPIRAM));
    if (!idat_buf) {
        idat_buf = static_cast<uint8_t*>(malloc(idat_total));
    }
    if (!idat_buf) {
        ESP_LOGE(TAG_PNG, "IDAT alloc failed (%u bytes)", idat_total);
        return -7;
    }
    {
        uint32_t off = 0;
        for (int i = 0; i < idat_count; ++i) {
            std::memcpy(idat_buf + off, idats[i].ptr, idats[i].len);
            off += idats[i].len;
        }
    }

    // 4. Inflate decompression
    // Bytes per pixel for each color type
    static constexpr int kBpp[] = {1, 0, 3, 1, 2, 0, 4}; // indexed by color_type
    int bpp = (info.color_type <= 6) ? kBpp[info.color_type] : 0;
    if (bpp == 0) {
        ESP_LOGE(TAG_PNG, "unsupported color type %d", info.color_type);
        free(idat_buf);
        return -8;
    }

    uint32_t stride = info.width * bpp;
    size_t raw_size = size_t(info.height) * (stride + 1); // +1 filter byte per row
    // Safety: cap at 32 MB
    if (raw_size > 32u * 1024u * 1024u) {
        ESP_LOGE(TAG_PNG, "raw data too large: %u", (unsigned)raw_size);
        free(idat_buf);
        return -9;
    }

    uint8_t* raw = static_cast<uint8_t*>(
        heap_caps_malloc(raw_size, MALLOC_CAP_SPIRAM));
    if (!raw) {
        ESP_LOGE(TAG_PNG, "raw buffer alloc failed (%u bytes)",
                 (unsigned)raw_size);
        free(idat_buf);
        return -10;
    }

    size_t decompressed = raw_size;
    int zret = inflate_zlib(idat_buf, idat_total, raw, &decompressed);
    free(idat_buf);

    if (zret != 0) {
        ESP_LOGE(TAG_PNG, "inflate failed: %d", zret);
        heap_caps_free(raw);
        return -11;
    }

    // 5. Un-filter scanlines (in-place, row by row)
    uint8_t* prev_row = nullptr;
    for (uint32_t y = 0; y < info.height; ++y) {
        uint8_t* row = raw + y * (stride + 1);
        if (!unfilter_scanline(row, prev_row, bpp, info.width)) {
            heap_caps_free(raw);
            return -12;
        }
        prev_row = row + 1; // pixel data (skip filter byte)
    }

    // 6. Convert to RGBA8888
    // SPIRAM — a 1024x1024 image needs 4 MB, far beyond internal RAM.
    size_t rgba_size = size_t(info.width) * info.height * 4;
    uint8_t* rgba = static_cast<uint8_t*>(
        heap_caps_malloc(rgba_size, MALLOC_CAP_SPIRAM));
    if (!rgba) {
        rgba = static_cast<uint8_t*>(malloc(rgba_size));
    }
    if (!rgba) {
        ESP_LOGE(TAG_PNG, "RGBA buffer alloc failed");
        heap_caps_free(raw);
        return -13;
    }

    for (uint32_t y = 0; y < info.height; ++y) {
        const uint8_t* src_row = raw + y * (stride + 1) + 1; // skip filter
        uint8_t* dst_row = rgba + y * info.width * 4;
        convert_to_rgba(src_row, dst_row, info.width,
                        info.color_type, palette, plte_entries,
                        trns, trns_len);
    }

    heap_caps_free(raw);

    *out = rgba;
    *w = info.width;
    *h = info.height;

    ESP_LOGI(TAG_PNG, "decoded %ux%u RGBA (%u bytes)", info.width, info.height,
             (unsigned)rgba_size);
    return 0;
}

} // namespace embed::display
