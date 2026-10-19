#!/usr/bin/env python3
"""Export trained KeywordSpotter to a binary blob + small C header.

Generates:
    wakeword_weights.bin  — packed int8 weights (flash via model partition)
    wakeword_model.h      — loader + inference function (tiny, no weight data)

Usage:
    python export_model.py [--model data/model/best_model.pt]
                           [--output-bin wakeword_weights.bin]
                           [--output-header wakeword_model.h]
"""
import argparse
import struct
import sys
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn

# Must match train_model.py
N_MELS = 40
N_FRAMES = 99
BUF_SIZE = 32 * 99 * 40  # max intermediate tensor elements
CW_WINDOW_LEN = 480       # DFT window: 30 ms at 16 kHz


class DSConv1d(nn.Module):
    def __init__(self, in_ch, out_ch, kernel=3, stride=1, pad=1):
        super().__init__()
        self.depthwise = nn.Conv1d(in_ch, in_ch, kernel, stride, pad,
                                   groups=in_ch)
        self.pointwise = nn.Conv1d(in_ch, out_ch, 1)
        self.bn = nn.BatchNorm1d(out_ch)

    def forward(self, x):
        x = self.depthwise(x)
        x = self.pointwise(x)
        x = self.bn(x)
        return torch.relu(x)


class KeywordSpotter(nn.Module):
    def __init__(self, n_mels=N_MELS, n_frames=N_FRAMES, n_classes=2):
        super().__init__()
        self.conv_init = nn.Sequential(
            nn.Conv2d(1, 32, kernel_size=(3, 3), padding=(1, 1)),
            nn.BatchNorm2d(32),
            nn.ReLU(),
        )
        self.feat_dim = 32 * n_mels
        self.ds1 = DSConv1d(self.feat_dim, 128, 3, 1, 1)
        self.ds2 = DSConv1d(128, 64, 3, 1, 1)
        self.ds3 = DSConv1d(64, 32, 3, 1, 1)
        self.classifier = nn.Sequential(
            nn.Dropout(0.3),
            nn.Linear(32, n_classes),
        )

    def forward(self, x):
        x = self.conv_init(x)
        B, C, T, F_dim = x.shape
        x = x.reshape(B, C * F_dim, T)
        x = self.ds1(x)
        x = self.ds2(x)
        x = self.ds3(x)
        x = x.mean(dim=2)
        x = self.classifier(x)
        return x


def quantize_weights(weights: np.ndarray):
    """Symmetric int8 quantisation. Returns (q_weights, scale)."""
    abs_max = np.max(np.abs(weights))
    if abs_max < 1e-10:
        return np.zeros_like(weights, dtype=np.int8), 1.0
    scale = abs_max / 127.0
    q = np.clip(np.round(weights / scale), -128, 127).astype(np.int8)
    return q, float(scale)


def extract_conv_params(module: nn.Conv1d):
    w = module.weight.detach().cpu().numpy()
    b = module.bias.detach().cpu().numpy() if module.bias is not None else None
    return w, b


def extract_bn_params(module: nn.BatchNorm1d):
    return (
        module.running_mean.detach().cpu().numpy(),
        module.running_var.detach().cpu().numpy(),
        module.weight.detach().cpu().numpy(),
        module.bias.detach().cpu().numpy(),
    )


def fold_conv_bn(conv_w, conv_b, bn_mean, bn_var, bn_weight, bn_bias,
                 eps=1e-5):
    std = np.sqrt(bn_var + eps)
    scale = bn_weight / std
    if conv_w.ndim == 4:
        s = scale.reshape(-1, 1, 1, 1)
    elif conv_w.ndim == 3:
        s = scale.reshape(-1, 1, 1)
    else:
        s = scale.reshape(-1, 1)
    folded_w = conv_w * s
    folded_b = (conv_b - bn_mean) * scale + bn_bias
    return folded_w, folded_b


# ── Binary format ────────────────────────────────────────────────────────────
#
# Header (48 bytes, little-endian):
#   magic     4s   "WWMD"
#   version   u32  = 1
#   n_blobs   u32  = 10
#   buf_size  u32  = 126720  (inference buffer size in floats)
#   data_off  u32  = offset to first data byte = 48 + n_blobs*12
#   reserved  4×u32
#
# Directory (n_blobs × 12 bytes each):
#   offset    u32  absolute offset in file
#   size      u32  bytes
#   scale     f32  dequantisation scale
#
# Blob order:
#   0  conv_init_w   (288)
#   1  conv_init_b   (32)
#   2  ds0_dw_w      (3840)
#   3  ds0_dw_b      (1280)
#   4  ds0_pw_w      (163840)
#   5  ds0_pw_b      (128)
#   6  ds1_dw_w      (384)
#   7  ds1_dw_b      (128)
#   8  ds1_pw_w      (8192)
#   9  ds1_pw_b      (64)
#  10  ds2_dw_w      (192)
#  11  ds2_dw_b      (64)
#  12  ds2_pw_w      (2048)
#  13  ds2_pw_b      (32)
#  14  cls_w         (64)
#  15  cls_b         (2)

N_BLOBS = 16
HDR_SIZE = 48
DIR_ENTRY = 12


def generate_bin(model: KeywordSpotter, bin_path: Path):
    """Generate the packed binary weights file."""
    model.eval()

    # ── Extract & fold ───────────────────────────────────────────────
    conv_init_w, conv_init_b = extract_conv_params(model.conv_init[0])
    bn_mean, bn_var, bn_w, bn_b = extract_bn_params(model.conv_init[1])
    conv_init_w, conv_init_b = fold_conv_bn(
        conv_init_w, conv_init_b, bn_mean, bn_var, bn_w, bn_b)

    layers = []
    for ds in [model.ds1, model.ds2, model.ds3]:
        dw_w, dw_b = extract_conv_params(ds.depthwise)
        pw_w, pw_b = extract_conv_params(ds.pointwise)
        bn_mean, bn_var, bn_w, bn_b = extract_bn_params(ds.bn)
        pw_w, pw_b = fold_conv_bn(pw_w, pw_b, bn_mean, bn_var, bn_w, bn_b)
        layers.append((dw_w, dw_b, pw_w, pw_b))

    cls_w = model.classifier[1].weight.detach().cpu().numpy()
    cls_b = model.classifier[1].bias.detach().cpu().numpy()

    # ── Quantise & collect blobs ─────────────────────────────────────
    blobs = []  # list of (int8_array, scale)

    def qadd(arr):
        q, s = quantize_weights(arr.flatten())
        blobs.append((q, s))

    qadd(conv_init_w)       # 0
    qadd(conv_init_b)       # 1
    for dw_w, dw_b, pw_w, pw_b in layers:
        qadd(dw_w)          # 2, 6, 10
        qadd(dw_b)          # 3, 7, 11
        qadd(pw_w)          # 4, 8, 12
        qadd(pw_b)          # 5, 9, 13
    qadd(cls_w)             # 14
    qadd(cls_b)             # 15

    assert len(blobs) == N_BLOBS

    # ── Compute offsets ──────────────────────────────────────────────
    data_off = HDR_SIZE + N_BLOBS * DIR_ENTRY
    offsets = []
    cur = data_off
    for q, _ in blobs:
        offsets.append(cur)
        cur += len(q)
    total_size = cur

    # ── Write binary ─────────────────────────────────────────────────
    with open(bin_path, 'wb') as f:
        # Header
        f.write(struct.pack('<4sIIIII',
                            b'WWMD', 1, N_BLOBS, BUF_SIZE, data_off, 0))
        f.write(struct.pack('<III', 0, 0, 0))  # 3 more reserved u32

        # Directory
        for i, (q, s) in enumerate(blobs):
            f.write(struct.pack('<IIf', offsets[i], len(q), s))

        # Data
        for q, _ in blobs:
            f.write(q.tobytes())

    print(f"Generated {bin_path}  ({total_size:,} bytes)")
    print(f"  Header: {HDR_SIZE}B, Dir: {N_BLOBS * DIR_ENTRY}B, "
          f"Data: {total_size - data_off:,}B")
    for i, (q, s) in enumerate(blobs):
        print(f"  blob[{i:2d}]: {len(q):6d} bytes  scale={s:.10f}")


# ── C header generation ──────────────────────────────────────────────────────

_HEADER = r'''// Auto-generated by export_model.py — DO NOT EDIT
// Wake word model: weights loaded from flash partition at runtime.
// Inference buffers allocated from PSRAM via heap_caps_malloc.

#pragma once

#include <stdint.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_log.h"

#define WW_N_MELS      %(n_mels)d
#define WW_N_FRAMES    %(n_frames)d
#define WW_N_CLASSES   2
#define WW_BUF_SIZE    %(buf_size)d   // max intermediate tensor (floats)
#define WW_N_BLOBS     %(n_blobs)d
#define WW_MAGIC       0x444D5757u    // "WWMD"

// Blob indices
enum {
    WW_CI_W = 0, WW_CI_B,
    WW_DS0_DW_W, WW_DS0_DW_B, WW_DS0_PW_W, WW_DS0_PW_B,
    WW_DS1_DW_W, WW_DS1_DW_B, WW_DS1_PW_W, WW_DS1_PW_B,
    WW_DS2_DW_W, WW_DS2_DW_B, WW_DS2_PW_W, WW_DS2_PW_B,
    WW_CLS_W, WW_CLS_B,
};

typedef struct {
    uint32_t offset;
    uint32_t size;
    float    scale;
} ww_blob_dir_t;

typedef struct {
    const int8_t   *blobs[WW_N_BLOBS];   // pointers into mmap'd flash
    float           scales[WW_N_BLOBS];
    float          *buf_a;                // PSRAM inference buffer
    float          *buf_b;                // PSRAM inference buffer
} ww_model_t;

static const char *WW_TAG = "wakeword";

/// Load model from "model" partition.
/// part_offset: byte offset of wakeword weights within the partition.
/// Returns 0 on success.
static inline int ww_model_load(ww_model_t *m, uint32_t part_offset)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "custom_model");
    if (!part) {
        ESP_LOGE(WW_TAG, "model partition not found");
        return -1;
    }

    // Read header
    uint8_t hdr[48];
    if (esp_partition_read(part, part_offset, hdr, sizeof(hdr)) != ESP_OK)
        return -1;

    uint32_t magic, ver, n_blobs, buf_sz, data_off;
    memcpy(&magic,  hdr +  0, 4);
    memcpy(&ver,    hdr +  4, 4);
    memcpy(&n_blobs,hdr +  8, 4);
    memcpy(&buf_sz, hdr + 12, 4);
    memcpy(&data_off,hdr + 16, 4);

    if (magic != WW_MAGIC || ver != 1 || n_blobs != WW_N_BLOBS) {
        ESP_LOGE(WW_TAG, "bad header: magic=0x%%08x ver=%%u n=%%u",
                 magic, ver, n_blobs);
        return -1;
    }

    // Read directory
    ww_blob_dir_t dir[WW_N_BLOBS];
    if (esp_partition_read(part, part_offset + 48,
                           dir, sizeof(dir)) != ESP_OK)
        return -1;

    // Memory-map the data region
    const void *mapped = NULL;
    esp_partition_mmap_handle_t handle;
    uint32_t data_size = part->size - (part_offset + data_off);
    // Align offset to 64KB boundary for mmap
    uint32_t map_offset = (part_offset + data_off) & ~0xFFFFu;
    uint32_t map_shift  = (part_offset + data_off) - map_offset;
    if (esp_partition_mmap(part, map_offset,
                           data_size + map_shift,
                           ESP_PARTITION_MMAP_DATA,
                           &mapped, &handle) != ESP_OK) {
        ESP_LOGE(WW_TAG, "mmap failed");
        return -1;
    }

    const int8_t *base = (const int8_t *)mapped + map_shift;
    for (int i = 0; i < WW_N_BLOBS; i++) {
        m->blobs[i]  = base + (dir[i].offset - data_off);
        m->scales[i] = dir[i].scale;
    }

    // Allocate inference buffers in PSRAM
    size_t buf_bytes = WW_BUF_SIZE * sizeof(float);
    m->buf_a = (float *)heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    m->buf_b = (float *)heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    if (!m->buf_a || !m->buf_b) {
        ESP_LOGE(WW_TAG, "PSRAM alloc failed (need 2×%%u bytes)",
                 (unsigned)buf_bytes);
        return -1;
    }

    ESP_LOGI(WW_TAG, "loaded: %%u blobs, %%.1fKB PSRAM",
             n_blobs, buf_bytes * 2.0f / 1024.0f);
    return 0;
}

static inline void ww_model_free(ww_model_t *m)
{
    free(m->buf_a);
    free(m->buf_b);
    m->buf_a = m->buf_b = NULL;
}

/// Run inference.  input: [WW_N_FRAMES][WW_N_MELS] float32.
/// output: [WW_N_CLASSES] float32 logits.
static inline void wakeword_infer(ww_model_t *m,
                                  const float *input, float *output)
{
    #define _CONV_OUT  m->buf_a
    #define _DS_OUT    m->buf_a
    #define _POOL      m->buf_b
    #define _CUR       m->buf_b
    #define _BLOB(idx) m->blobs[idx]
    #define _SC(idx)   m->scales[idx]

    /* 1. conv_init (1,99,40) -> (32,99,40) */
    for (int oc = 0; oc < 32; oc++) {
        for (int t = 0; t < 99; t++) {
            for (int f = 0; f < 40; f++) {
                float v = (float)_BLOB(WW_CI_B)[oc] * _SC(WW_CI_B);
                for (int kt = 0; kt < 3; kt++) {
                    for (int kf = 0; kf < 3; kf++) {
                        int it = t + kt - 1;
                        int iF = f + kf - 1;
                        if (it >= 0 && it < 99 && iF >= 0 && iF < 40)
                            v += (float)_BLOB(WW_CI_W)[((oc)*3+kt)*3+kf]
                                 * _SC(WW_CI_W) * input[it*40+iF];
                    }
                }
                _CONV_OUT[(oc*99+t)*40+f] = v > 0.0f ? v : 0.0f;
            }
        }
    }

    /* 2. Reshape (32,99,40) -> (1280,99) */
    for (int c = 0; c < 32; c++)
        for (int t = 0; t < 99; t++)
            for (int f = 0; f < 40; f++)
                _CUR[(c*40+f)*99+t] = _CONV_OUT[(c*99+t)*40+f];

    /* 3. DS-Conv blocks */
'''


def _ds_block(lines, blk, ci, co):
    """Emit C code for one DS-Conv block."""
    dw_w = f"WW_DS{blk}_DW_W"
    dw_b = f"WW_DS{blk}_DW_B"
    pw_w = f"WW_DS{blk}_PW_W"
    pw_b = f"WW_DS{blk}_PW_B"
    lines.append(f"    /* Block {blk}: {ci} -> {co} */")
    lines.append(f"    for (int c = 0; c < {ci}; c++) {{")
    lines.append(f"        for (int t = 0; t < 99; t++) {{")
    lines.append(f"            float v = 0.0f;")
    lines.append(f"            for (int k = 0; k < 3; k++) {{")
    lines.append(f"                int idx = t + k - 1;")
    lines.append(f"                if (idx >= 0 && idx < 99)")
    lines.append(f"                    v += (float)_BLOB({dw_w})[c*3+k] * _SC({dw_w});")
    lines.append(f"            }}")
    lines.append(f"            v += (float)_BLOB({dw_b})[c] * _SC({dw_b});")
    lines.append(f"            _DS_OUT[c*99+t] = v;")
    lines.append(f"        }}")
    lines.append(f"    }}")
    lines.append(f"    for (int oc = 0; oc < {co}; oc++) {{")
    lines.append(f"        for (int t = 0; t < 99; t++) {{")
    lines.append(f"            float v = (float)_BLOB({pw_b})[oc] * _SC({pw_b});")
    lines.append(f"            for (int ic = 0; ic < {ci}; ic++)")
    lines.append(f"                v += (float)_BLOB({pw_w})[oc*{ci}+ic] * _SC({pw_w}) * _DS_OUT[ic*99+t];")
    lines.append(f"            _CUR[oc*99+t] = v > 0.0f ? v : 0.0f;")
    lines.append(f"        }}")
    lines.append(f"    }}")
    lines.append("")


_FOOTER = r'''
    /* 4. Global avg pool */
    for (int c = 0; c < 32; c++) {
        float s = 0.0f;
        for (int t = 0; t < 99; t++) s += _CUR[c*99+t];
        _POOL[c] = s / 99.0f;
    }

    /* 5. Classifier Linear(32->2) */
    for (int c = 0; c < 2; c++) {
        float v = (float)_BLOB(WW_CLS_B)[c] * _SC(WW_CLS_B);
        for (int i = 0; i < 32; i++)
            v += (float)_BLOB(WW_CLS_W)[c*32+i] * _SC(WW_CLS_W) * _POOL[i];
        output[c] = v;
    }

    #undef _CONV_OUT
    #undef _DS_OUT
    #undef _POOL
    #undef _CUR
    #undef _BLOB
    #undef _SC
}
'''


def generate_dft_tables_c(window_len: int) -> str:
    """Generate C const arrays for DFT twiddle factors and Hann window.

    These are compile-time constants baked into flash (.rodata).
    No runtime allocation needed.
    """
    two_pi = 2.0 * np.pi
    lines = []
    lines.append("// ── Precomputed DFT tables (compile-time constants in flash) ──")
    lines.append("#define CW_DFT_PRECOMPUTED")
    lines.append(f"#define CW_WINDOW_LEN  {window_len}")
    lines.append("")

    # cos(2π·i/N)
    cos_vals = np.cos(two_pi * np.arange(window_len) / window_len).astype(np.float32)
    lines.append(f"static const float cw_cos_twiddle[{window_len}] = {{")
    for i in range(0, window_len, 8):
        chunk = ", ".join(f"{v:.8f}f" for v in cos_vals[i:i+8])
        lines.append(f"    {chunk},")
    lines.append("};")
    lines.append("")

    # sin(2π·i/N)
    sin_vals = np.sin(two_pi * np.arange(window_len) / window_len).astype(np.float32)
    lines.append(f"static const float cw_sin_twiddle[{window_len}] = {{")
    for i in range(0, window_len, 8):
        chunk = ", ".join(f"{v:.8f}f" for v in sin_vals[i:i+8])
        lines.append(f"    {chunk},")
    lines.append("};")
    lines.append("")

    # Hann window: 0.5 * (1 - cos(2π·i/(N-1)))
    hann_vals = (0.5 * (1.0 - np.cos(two_pi * np.arange(window_len) / (window_len - 1)))).astype(np.float32)
    lines.append(f"static const float cw_hann_window[{window_len}] = {{")
    for i in range(0, window_len, 8):
        chunk = ", ".join(f"{v:.8f}f" for v in hann_vals[i:i+8])
        lines.append(f"    {chunk},")
    lines.append("};")
    lines.append("")

    return "\n".join(lines)


def generate_header(header_path: Path):
    """Generate the small C header (no weight data) + DFT tables."""
    lines = []
    lines.append(_HEADER % dict(
        n_mels=N_MELS, n_frames=N_FRAMES,
        buf_size=BUF_SIZE, n_blobs=N_BLOBS))

    ds_channels = [(1280, 128), (128, 64), (64, 32)]
    for blk, (ci, co) in enumerate(ds_channels):
        _ds_block(lines, blk, ci, co)

    lines.append(_FOOTER)

    # Append precomputed DFT twiddle factors + Hann window
    lines.append("\n")
    lines.append(generate_dft_tables_c(CW_WINDOW_LEN))

    with open(header_path, 'w') as f:
        f.write(''.join(lines))
    print(f"Generated {header_path}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", default="data/model/best_model.pt")
    parser.add_argument("--output-bin", default="wakeword_weights.bin")
    parser.add_argument("--output-header", default=None,
                        help="C header path (default: same dir as output-bin)")
    args = parser.parse_args()

    model_path = Path(args.model)
    if not model_path.exists():
        print(f"ERROR: model not found: {model_path}")
        print("Run train_model.py first.")
        return 1

    model = KeywordSpotter()
    model.load_state_dict(torch.load(str(model_path), map_location='cpu'))
    model.eval()

    bin_path = Path(args.output_bin)
    generate_bin(model, bin_path)

    header_path = args.output_header
    if header_path is None:
        header_path = bin_path.with_name("wakeword_model.h")
    generate_header(Path(header_path))

    return 0


if __name__ == "__main__":
    sys.exit(main())
