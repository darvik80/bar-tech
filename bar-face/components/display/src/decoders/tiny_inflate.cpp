/// Minimal inflate (RFC 1951 DEFLATE + RFC 1950 zlib wrapper).
/// Based on the DEFLATE specification and tinf by Joergen Ibsen (zlib license).

#include "tiny_inflate.hpp"
#include <cstring>

namespace embed::display {

// ── Fixed Huffman tables ───────────────────────────────────────────────

static const unsigned char kFixedLitLens[288] = {
    // 0-143: 8 bits (144 symbols)
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    // 144-255: 9 bits (112 symbols)
    9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
    9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
    9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
    9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
    // 256-279: 7 bits (24 symbols)
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    // 280-287: 8 bits (8 symbols)
    8,8,8,8,8,8,8,8
};

static const unsigned char kFixedDistLens[30] = {
    5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5
};

// Length base values and extra bits for literal/length codes 257-285
static const uint16_t kLenBase[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,
    67,83,99,115,131,163,195,227,258
};
static const uint8_t kLenExtra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};

// Distance base values and extra bits
static const uint16_t kDistBase[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
    1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const uint8_t kDistExtra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

// Code length order for dynamic Huffman
static const uint8_t kClOrder[19] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

// ── Huffman decoder ────────────────────────────────────────────────────

static constexpr int kMaxSym  = 288;
static constexpr int kMaxBits = 15;

struct HuffTable {
    uint16_t counts[kMaxBits + 1];   // number of codes of each length
    uint16_t symbols[kMaxSym];       // canonically ordered symbols
};

/// Build Huffman table from code lengths.
static bool build_table(HuffTable* h, const uint8_t* lengths, int nsyms)
{
    std::memset(h->counts, 0, sizeof(h->counts));

    for (int i = 0; i < nsyms; ++i)
        h->counts[lengths[i]]++;

    // Check for no codes at all
    h->counts[0] = 0;

    // Compute offsets
    uint16_t offsets[kMaxBits + 1];
    offsets[0] = 0;
    for (int i = 1; i <= kMaxBits; ++i)
        offsets[i] = offsets[i - 1] + h->counts[i - 1];

    for (int i = 0; i < nsyms; ++i) {
        if (lengths[i] != 0)
            h->symbols[offsets[lengths[i]]++] = static_cast<uint16_t>(i);
    }
    return true;
}

// ── Bit reader ─────────────────────────────────────────────────────────

struct BitReader {
    const uint8_t* src;
    size_t         src_len;
    size_t         pos;
    uint32_t       bit_buf;
    int            bit_count;
};

static void br_init(BitReader* br, const uint8_t* src, size_t len)
{
    br->src = src;
    br->src_len = len;
    br->pos = 0;
    br->bit_buf = 0;
    br->bit_count = 0;
}

static int br_bits(BitReader* br, int n)
{
    while (br->bit_count < n) {
        if (br->pos >= br->src_len) return -1;
        br->bit_buf |= uint32_t(br->src[br->pos++]) << br->bit_count;
        br->bit_count += 8;
    }
    int val = int(br->bit_buf) & ((1 << n) - 1);
    br->bit_buf >>= n;
    br->bit_count -= n;
    return val;
}

/// Decode one Huffman symbol.
static int decode_symbol(BitReader* br, const HuffTable* h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= kMaxBits; ++len) {
        code |= br_bits(br, 1);
        if (code < 0) return -1;
        int count = h->counts[len];
        if (code - count < first) {
            return h->symbols[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1; // bad code
}

// ── Decompress one block ──────────────────────────────────────────────

static int decompress_block(BitReader* br, HuffTable* lit, HuffTable* dist,
                            uint8_t* dst, size_t dst_cap, size_t* out_pos)
{
    while (true) {
        int sym = decode_symbol(br, lit);
        if (sym < 0) return -20;

        if (sym < 256) {
            // Literal byte
            if (*out_pos >= dst_cap) return -21;
            dst[(*out_pos)++] = static_cast<uint8_t>(sym);
        }
        else if (sym == 256) {
            // End of block
            return 0;
        }
        else {
            // Length/distance pair
            int len_idx = sym - 257;
            if (len_idx < 0 || len_idx >= 29) return -22;

            int extra = br_bits(br, kLenExtra[len_idx]);
            if (extra < 0) return -23;
            int length = kLenBase[len_idx] + extra;

            int dsym = decode_symbol(br, dist);
            if (dsym < 0 || dsym >= 30) return -24;

            int dextra = br_bits(br, kDistExtra[dsym]);
            if (dextra < 0) return -25;
            int distance = kDistBase[dsym] + dextra;

            // Copy from back-reference
            if (distance > int(*out_pos)) return -26;
            size_t src_pos = *out_pos - distance;
            for (int i = 0; i < length; ++i) {
                if (*out_pos >= dst_cap) return -27;
                dst[*out_pos] = dst[src_pos];
                (*out_pos)++;
                src_pos++;
            }
        }
    }
}

// ── Build dynamic Huffman tables ───────────────────────────────────────

static int build_dynamic_tables(BitReader* br,
                                 HuffTable* lit, HuffTable* dist)
{
    int nlit = br_bits(br, 5) + 257;
    int ndist = br_bits(br, 5) + 1;
    int nclen = br_bits(br, 4) + 4;
    if (nlit > 286 || ndist > 30 || nclen > 19) return -30;

    // Read code-length code lengths
    uint8_t cl_lens[19] = {};
    for (int i = 0; i < nclen; ++i) {
        int v = br_bits(br, 3);
        if (v < 0) return -31;
        cl_lens[kClOrder[i]] = static_cast<uint8_t>(v);
    }

    HuffTable cl_table{};
    build_table(&cl_table, cl_lens, 19);

    // Decode literal/length + distance code lengths
    uint8_t lengths[288 + 32] = {};
    int total = nlit + ndist;
    int i = 0;
    while (i < total) {
        int sym = decode_symbol(br, &cl_table);
        if (sym < 0) return -32;

        if (sym < 16) {
            lengths[i++] = static_cast<uint8_t>(sym);
        }
        else if (sym == 16) {
            int rep = br_bits(br, 2) + 3;
            if (rep < 0) return -33;
            uint8_t prev = (i > 0) ? lengths[i - 1] : 0;
            for (int j = 0; j < rep && i < total; ++j)
                lengths[i++] = prev;
        }
        else if (sym == 17) {
            int rep = br_bits(br, 3) + 3;
            if (rep < 0) return -34;
            for (int j = 0; j < rep && i < total; ++j)
                lengths[i++] = 0;
        }
        else if (sym == 18) {
            int rep = br_bits(br, 7) + 11;
            if (rep < 0) return -35;
            for (int j = 0; j < rep && i < total; ++j)
                lengths[i++] = 0;
        }
        else {
            return -36;
        }
    }

    build_table(lit, lengths, nlit);
    build_table(dist, lengths + nlit, ndist);
    return 0;
}

// ── Public API ─────────────────────────────────────────────────────────

int inflate_zlib(const uint8_t* src, size_t src_len,
                 uint8_t* dst, size_t* dst_len)
{
    if (src_len < 2) return -1;

    // Skip zlib header (2 bytes: CMF + FLG)
    uint8_t cmf = src[0];
    uint8_t flg = src[1];

    // Verify: CMF*256 + FLG must be multiple of 31
    if ((uint16_t(cmf) * 256 + flg) % 31 != 0) return -2;
    // CM = 8 (deflate)
    if ((cmf & 0x0F) != 8) return -3;

    BitReader br{};
    br_init(&br, src + 2, src_len - 2);

    size_t out_pos = 0;
    size_t cap = *dst_len;

    HuffTable lit_table{}, dist_table{};
    bool final_block = false;

    while (!final_block) {
        int bfinal = br_bits(&br, 1);
        int btype  = br_bits(&br, 2);
        if (bfinal < 0 || btype < 0) return -10;

        final_block = (bfinal == 1);

        if (btype == 0) {
            // Stored (uncompressed) block
            // Align to byte boundary
            br.bit_buf = 0;
            br.bit_count = 0;
            if (br.pos + 4 > br.src_len) return -11;

            uint16_t len  = uint16_t(br.src[br.pos]) |
                            (uint16_t(br.src[br.pos + 1]) << 8);
            br.pos += 2;
            uint16_t nlen = uint16_t(br.src[br.pos]) |
                            (uint16_t(br.src[br.pos + 1]) << 8);
            br.pos += 2;

            if (len != uint16_t(~nlen)) return -12;
            if (br.pos + len > br.src_len) return -13;
            if (out_pos + len > cap) return -14;

            std::memcpy(dst + out_pos, br.src + br.pos, len);
            br.pos += len;
            out_pos += len;
        }
        else if (btype == 1) {
            // Fixed Huffman codes
            build_table(&lit_table, kFixedLitLens, 288);
            build_table(&dist_table, kFixedDistLens, 30);
            int err = decompress_block(&br, &lit_table, &dist_table,
                                       dst, cap, &out_pos);
            if (err != 0) return err;
        }
        else if (btype == 2) {
            // Dynamic Huffman codes
            int err = build_dynamic_tables(&br, &lit_table, &dist_table);
            if (err != 0) return err;
            err = decompress_block(&br, &lit_table, &dist_table,
                                   dst, cap, &out_pos);
            if (err != 0) return err;
        }
        else {
            return -15; // reserved BTYPE
        }
    }

    *dst_len = out_pos;
    return 0;
}

} // namespace embed::display
