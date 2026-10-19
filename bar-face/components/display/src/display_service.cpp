#include "display/display_service.hpp"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_types.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "decoders/png_decoder.hpp"
#include "embedded_images.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static constexpr char TAG[] = "Display";

// ── Pin mapping (from Kconfig) ─────────────────────────────────────────

static constexpr int kWidth = 240;
static constexpr int kHeight = 240;

static constexpr gpio_num_t kSck  = static_cast<gpio_num_t>(CONFIG_EMBED_DISPLAY_SPI_SCK);
static constexpr gpio_num_t kMosi = static_cast<gpio_num_t>(CONFIG_EMBED_DISPLAY_SPI_MOSI);
static constexpr gpio_num_t kDc   = static_cast<gpio_num_t>(CONFIG_EMBED_DISPLAY_PIN_DC);
static constexpr gpio_num_t kCs   = static_cast<gpio_num_t>(CONFIG_EMBED_DISPLAY_PIN_CS);
static constexpr gpio_num_t kRst  = static_cast<gpio_num_t>(CONFIG_EMBED_DISPLAY_PIN_RST);
static constexpr gpio_num_t kBlk  = static_cast<gpio_num_t>(CONFIG_EMBED_DISPLAY_PIN_BLK);

// ── Minimal 8×8 bitmap font (ASCII 32–127) ────────────────────────────
// Each glyph = 8 bytes; bit 7 = leftmost pixel.

static const uint8_t kFont8x8[96][8] = {
    {0, 0, 0, 0, 0, 0, 0, 0}, // 32 space
    {0x18, 0x3C, 0x3C, 0x18, 0x18, 0, 0x18, 0}, // 33 !
    {0x36, 0x36, 0, 0, 0, 0, 0, 0}, // 34 "
    {0x36, 0x7F, 0x36, 0x7F, 0x36, 0, 0, 0}, // 35 #
    {0x0C, 0x3E, 0x03, 0x1E, 0x03, 0x7C, 0x18, 0}, // 36 $
    {0, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0}, // 37 %
    {0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0}, // 38 &
    {0x06, 0x03, 0x03, 0, 0, 0, 0, 0}, // 39 '
    {0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0}, // 40 (
    {0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0}, // 41 )
    {0, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0, 0}, // 42 *
    {0, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0, 0}, // 43 +
    {0, 0, 0, 0, 0, 0x0C, 0x0C, 0x06}, // 44 ,
    {0, 0, 0, 0x3F, 0, 0, 0, 0}, // 45 -
    {0, 0, 0, 0, 0, 0, 0x0C, 0}, // 46 .
    {0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0}, // 47 /
    {0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0}, // 48 0
    {0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0}, // 49 1
    {0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0}, // 50 2
    {0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0}, // 51 3
    {0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0}, // 52 4
    {0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0}, // 53 5
    {0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0}, // 54 6
    {0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0}, // 55 7
    {0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0}, // 56 8
    {0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0}, // 57 9
    {0, 0x0C, 0x0C, 0, 0, 0x0C, 0x0C, 0}, // 58 :
    {0, 0x0C, 0x0C, 0, 0, 0x0C, 0x0C, 0x06}, // 59 ;
    {0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0}, // 60 <
    {0, 0, 0x3F, 0, 0, 0x3F, 0, 0}, // 61 =
    {0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0}, // 62 >
    {0x1E, 0x33, 0x30, 0x18, 0x0C, 0, 0x0C, 0}, // 63 ?
    {0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0}, // 64 @
    {0x0C, 0x1E, 0x33, 0x63, 0x7F, 0x63, 0x63, 0}, // 65 A
    {0x1F, 0x33, 0x33, 0x1F, 0x33, 0x33, 0x1F, 0}, // 66 B
    {0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0}, // 67 C
    {0x0F, 0x1B, 0x33, 0x63, 0x33, 0x1B, 0x0F, 0}, // 68 D
    {0x7F, 0x03, 0x03, 0x1F, 0x03, 0x03, 0x7F, 0}, // 69 E
    {0x7F, 0x03, 0x03, 0x1F, 0x03, 0x03, 0x03, 0}, // 70 F
    {0x3C, 0x66, 0x03, 0x73, 0x63, 0x66, 0x3C, 0}, // 71 G
    {0x63, 0x63, 0x63, 0x7F, 0x63, 0x63, 0x63, 0}, // 72 H
    {0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0}, // 73 I
    {0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0}, // 74 J
    {0x67, 0x6F, 0x1B, 0x03, 0x1B, 0x6F, 0x67, 0}, // 75 K
    {0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x7F, 0}, // 76 L
    {0x63, 0x77, 0x7F, 0x6B, 0x63, 0x63, 0x63, 0}, // 77 M
    {0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0}, // 78 N
    {0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0}, // 79 O
    {0x1F, 0x33, 0x33, 0x1F, 0x03, 0x03, 0x03, 0}, // 80 P
    {0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0}, // 81 Q
    {0x1F, 0x33, 0x33, 0x1F, 0x0B, 0x13, 0x33, 0}, // 82 R
    {0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0}, // 83 S
    {0x3F, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0}, // 84 T
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0}, // 85 U
    {0x63, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x08, 0}, // 86 V
    {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0}, // 87 W
    {0x63, 0x63, 0x36, 0x1C, 0x36, 0x63, 0x63, 0}, // 88 X
    {0x63, 0x63, 0x36, 0x1C, 0x0C, 0x0C, 0x0C, 0}, // 89 Y
    {0x7F, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x7F, 0}, // 90 Z
    {0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0}, // 91 [
    {0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0}, // 92 backslash
    {0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0}, // 93 ]
    {0x08, 0x1C, 0x36, 0x63, 0, 0, 0, 0}, // 94 ^
    {0, 0, 0, 0, 0, 0, 0, 0xFF}, // 95 _
    {0x0C, 0x0C, 0x18, 0, 0, 0, 0, 0}, // 96 `
    {0, 0, 0x1E, 0x30, 0x3E, 0x33, 0x6E, 0}, // 97 a
    {0x03, 0x03, 0x1F, 0x33, 0x33, 0x33, 0x1E, 0}, // 98 b
    {0, 0, 0x1E, 0x33, 0x03, 0x33, 0x1E, 0}, // 99 c
    {0x30, 0x30, 0x3E, 0x33, 0x33, 0x33, 0x6E, 0}, // 100 d
    {0, 0, 0x1E, 0x33, 0x3F, 0x03, 0x1E, 0}, // 101 e
    {0x1C, 0x36, 0x06, 0x0F, 0x06, 0x06, 0x06, 0}, // 102 f
    {0, 0, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x1F}, // 103 g
    {0x03, 0x03, 0x1B, 0x33, 0x33, 0x33, 0x33, 0}, // 104 h
    {0x0C, 0, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0}, // 105 i
    {0x18, 0, 0x18, 0x18, 0x18, 0x18, 0x33, 0x1E}, // 106 j
    {0x03, 0x03, 0x33, 0x1B, 0x0F, 0x1B, 0x33, 0}, // 107 k
    {0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0}, // 108 l
    {0, 0, 0x33, 0x7F, 0x7F, 0x6B, 0x63, 0}, // 109 m
    {0, 0, 0x1F, 0x33, 0x33, 0x33, 0x33, 0}, // 110 n
    {0, 0, 0x1E, 0x33, 0x33, 0x33, 0x1E, 0}, // 111 o
    {0, 0, 0x1F, 0x33, 0x33, 0x1F, 0x03, 0x03}, // 112 p
    {0, 0, 0x3E, 0x33, 0x33, 0x3E, 0x30, 0x30}, // 113 q
    {0, 0, 0x3B, 0x0F, 0x03, 0x03, 0x03, 0}, // 114 r
    {0, 0, 0x1E, 0x33, 0x0E, 0x33, 0x1E, 0}, // 115 s
    {0x0C, 0x0C, 0x3E, 0x0C, 0x0C, 0x2C, 0x18, 0}, // 116 t
    {0, 0, 0x33, 0x33, 0x33, 0x33, 0x6E, 0}, // 117 u
    {0, 0, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0}, // 118 v
    {0, 0, 0x63, 0x6B, 0x7F, 0x3E, 0x36, 0}, // 119 w
    {0, 0, 0x63, 0x36, 0x1C, 0x36, 0x63, 0}, // 120 x
    {0, 0, 0x33, 0x33, 0x33, 0x3E, 0x30, 0x1F}, // 121 y
    {0, 0, 0x3F, 0x18, 0x0C, 0x06, 0x3F, 0}, // 122 z
    {0x0E, 0x18, 0x18, 0x70, 0x18, 0x18, 0x0E, 0}, // 123 {
    {0x18, 0x18, 0x18, 0, 0x18, 0x18, 0x18, 0}, // 124 |
    {0x70, 0x18, 0x18, 0x0E, 0x18, 0x18, 0x70, 0}, // 125 }
    {0x76, 0xDC, 0, 0, 0, 0, 0, 0}, // 126 ~
};

// RGB565 colour helpers
static constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>(
        ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// 4x4 Bayer ordered-dither matrix (values 0..15). Used as a dither
// threshold when quantising 8-bit channels down to 5/6/5 — visually
// breaks the harsh banding you otherwise see on a 240x240 RGB565 panel
// for smooth gradients and dark areas.
static const uint8_t kBayer4x4[16] = {
     0,  8,  2, 10,
    12,  4, 14,  6,
     3, 11,  1,  9,
    15,  7, 13,  5,
};

/// Same as rgb565() but with a Bayer dither threshold keyed to the
/// destination (x, y) — this is the right choice for image content
/// where banding from 5/6/5 quantisation is otherwise very visible.
static constexpr uint16_t rgb565Dithered(uint8_t r, uint8_t g, uint8_t b,
                                         int x, int y)
{
    const int bayer = kBayer4x4[(y & 3) * 4 + (x & 3)];  // 0..15
    int r5 = (r * 31 + bayer * 17) / 255;  // 0..31
    int g6 = (g * 63 + bayer * 17) / 255;  // 0..63
    int b5 = (b * 31 + bayer * 17) / 255;  // 0..31
    if (r5 > 31) r5 = 31;
    if (g6 > 63) g6 = 63;
    if (b5 > 31) b5 = 31;
    return static_cast<uint16_t>((r5 << 11) | (g6 << 5) | b5);
}

namespace embed::display
{
    // ── Panel init ─────────────────────────────────────────────────────────

    bool DisplayService::initPanel()
    {
        width_ = kWidth;
        height_ = kHeight;

        // SPI bus
        spi_bus_config_t busCfg{};
        busCfg.sclk_io_num = kSck;
        busCfg.mosi_io_num = kMosi;
        busCfg.miso_io_num = GPIO_NUM_NC;
        busCfg.quadwp_io_num = GPIO_NUM_NC;
        busCfg.quadhd_io_num = GPIO_NUM_NC;
        busCfg.max_transfer_sz = kWidth * kHeight * sizeof(uint16_t);

        esp_err_t ret = spi_bus_initialize(SPI3_HOST, &busCfg, SPI_DMA_CH_AUTO);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
            return false;
        }

        // SPI panel IO
        esp_lcd_panel_io_handle_t ioHandle = nullptr;
        esp_lcd_panel_io_spi_config_t ioCfg{};
        ioCfg.dc_gpio_num = kDc;
        ioCfg.cs_gpio_num = kCs;
        ioCfg.pclk_hz = 80 * 1000 * 1000; // 40 MHz
        ioCfg.trans_queue_depth = 10;
        ioCfg.lcd_cmd_bits = 8;
        ioCfg.lcd_param_bits = 8;
        ioCfg.spi_mode = 3; // ST7789: try mode 3 if mode 0 doesn't work

        ret = esp_lcd_new_panel_io_spi(SPI3_HOST, &ioCfg, &ioHandle);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %s", esp_err_to_name(ret));
            spi_bus_free(SPI3_HOST);
            return false;
        }

        // ST7789 panel
        esp_lcd_panel_handle_t panelHandle = nullptr;
        esp_lcd_panel_dev_config_t panelCfg{};
        panelCfg.reset_gpio_num = kRst;
        panelCfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panelCfg.bits_per_pixel = 16;

        ret = esp_lcd_new_panel_st7789(ioHandle, &panelCfg, &panelHandle);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_lcd_new_panel_st7789 failed: %s", esp_err_to_name(ret));
            spi_bus_free(SPI3_HOST);
            return false;
        }

        ESP_ERROR_CHECK(esp_lcd_panel_reset(panelHandle));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panelHandle));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panelHandle, false));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panelHandle, false, false));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panelHandle, true));

        // Fill white before turning on to avoid garbage flash
        {
            uint16_t* whiteLine = static_cast<uint16_t*>(heap_caps_malloc(
                width_ * sizeof(uint16_t), MALLOC_CAP_DMA));
            if (whiteLine)
            {
                for (int i = 0; i < width_; ++i) whiteLine[i] = 0xFFFF;
                for (int y = 0; y < height_; ++y)
                {
                    esp_lcd_panel_draw_bitmap(panelHandle, 0, y, width_, y + 1,
                                              whiteLine);
                }
                heap_caps_free(whiteLine);
            } else
            {
                ESP_LOGW(TAG, "failed to allocate memory for white line");
            }
        }

        esp_lcd_panel_disp_on_off(panelHandle, true);
        panelHandle_ = static_cast<void*>(panelHandle);

        // Backlight (try active-low if active-high doesn't work)
        if (kBlk >= 0)
        {
            gpio_config_t ioGpioCfg{};
            ioGpioCfg.pin_bit_mask = (1ULL << kBlk);
            ioGpioCfg.mode = GPIO_MODE_OUTPUT;
            gpio_config(&ioGpioCfg);
            gpio_set_level(kBlk, 1); // active-low
            ESP_LOGI(TAG, "backlight ON (active-low, GPIO=%d)", kBlk);
        }

        ESP_LOGI(TAG, "ST7789 ready: %dx%d  SCK=%d MOSI=%d DC=%d CS=%d RST=%d BLK=%d",
                 width_, height_, kSck, kMosi, kDc, kCs, kRst, kBlk);

        return true;
    }

    // ── Fill screen with solid colour ──────────────────────────────────────

    void DisplayService::fillColour(uint16_t c)
    {
        if (!panelHandle_) return;
        auto* panel = static_cast<esp_lcd_panel_handle_t>(panelHandle_);

        // Fill one row at a time to keep buffer small
        uint16_t* line = static_cast<uint16_t*>(heap_caps_malloc(
            width_ * sizeof(uint16_t), MALLOC_CAP_DMA));
        if (!line) return;

        for (int i = 0; i < width_; ++i) line[i] = c;

        for (int y = 0; y < height_; ++y)
        {
            esp_lcd_panel_draw_bitmap(panel, 0, y, width_, y + 1,
                                      line);
        }
        heap_caps_free(line);
    }

    // ── Draw text (8×8 bitmap font) ────────────────────────────────────────

    void DisplayService::drawText(int x, int y, const char* text, uint16_t colour)
    {
        if (!panelHandle_ || !text) return;
        auto* panel = static_cast<esp_lcd_panel_handle_t>(panelHandle_);

        int cx = x;
        for (const char* p = text; *p; ++p)
        {
            if (*p == '\n')
            {
                cx = x;
                y += 8;
                continue;
            }
            int idx = static_cast<uint8_t>(*p) - 32;
            if (idx < 0 || idx >= 96) idx = 0;

            // Render glyph into a small buffer
            uint16_t buf[8 * 8];
            for (int row = 0; row < 8; ++row)
            {
                for (int col = 0; col < 8; ++col)
                {
                    buf[row * 8 + col] =
                        (kFont8x8[idx][row] >> (7 - col)) & 1 ? colour : 0;
                }
            }

            // Draw each row
            for (int row = 0; row < 8; ++row)
            {
                esp_lcd_panel_draw_bitmap(panel,
                                          cx, y + row, cx + 8, y + row + 1,
                                          &buf[row * 8]);
            }
            cx += 8;
        }
    }

    // ── Draw raw RGB565 bitmap ─────────────────────────────────────────────

    void DisplayService::drawBitmap(int x, int y, int w, int h,
                                    const uint16_t* rgb565)
    {
        if (!panelHandle_ || !rgb565) return;
        auto* panel = static_cast<esp_lcd_panel_handle_t>(panelHandle_);

        // Clip to screen bounds
        int x1 = x, y1 = y;
        int x2 = x + w, y2 = y + h;
        if (x1 < 0) x1 = 0;
        if (y1 < 0) y1 = 0;
        if (x2 > width_)  x2 = width_;
        if (y2 > height_) y2 = height_;
        if (x1 >= x2 || y1 >= y2) return;

        // Draw row-by-row from the source buffer
        for (int row = y1; row < y2; ++row)
        {
            int srcRow = row - y;
            const uint16_t* line = rgb565 + srcRow * w + (x1 - x);
            esp_lcd_panel_draw_bitmap(panel, x1, row, x2, row + 1, line);
        }
    }

    // ── Draw PNG from memory buffer ────────────────────────────────────────

    bool DisplayService::drawPng(int x, int y,
                                  const uint8_t* data, size_t size)
    {
        if (!panelHandle_ || !data || size == 0) return false;
        auto* panel = static_cast<esp_lcd_panel_handle_t>(panelHandle_);

        uint8_t* rgba = nullptr;
        uint32_t pw = 0, ph = 0;
        int err = png_decode_rgba(data, size, &rgba, &pw, &ph);
        if (err != 0 || !rgba)
        {
            ESP_LOGE(TAG, "PNG decode failed: %d", err);
            return false;
        }

        ESP_LOGI(TAG, "PNG decoded: %ux%u, drawing at (%d,%d)", pw, ph, x, y);

        // Allocate DMA line buffer (RGB565)
        uint16_t* line = static_cast<uint16_t*>(
            heap_caps_malloc(pw * sizeof(uint16_t), MALLOC_CAP_DMA));
        if (!line)
        {
            free(rgba);
            return false;
        }

        // Push RGBA → RGB565 line-by-line
        for (uint32_t row = 0; row < ph; ++row)
        {
            int ly = y + static_cast<int>(row);
            if (ly < 0 || ly >= height_) continue;

            const uint8_t* src = rgba + row * pw * 4;
            for (uint32_t col = 0; col < pw; ++col)
            {
                uint8_t r = src[col * 4 + 0];
                uint8_t g = src[col * 4 + 1];
                uint8_t b = src[col * 4 + 2];
                uint8_t a = src[col * 4 + 3];

                // Alpha-blend against white background
                if (a < 255)
                {
                    r = static_cast<uint8_t>((r * a + 255 * (255 - a)) / 255);
                    g = static_cast<uint8_t>((g * a + 255 * (255 - a)) / 255);
                    b = static_cast<uint8_t>((b * a + 255 * (255 - a)) / 255);
                }
                line[col] = rgb565Dithered(r, g, b, col, row);
            }

            int lx = x;
            int x2 = lx + static_cast<int>(pw);
            if (x2 > width_) x2 = width_;
            if (lx < 0) lx = 0;
            if (lx < x2)
            {
                esp_lcd_panel_draw_bitmap(panel, lx, ly, x2, ly + 1,
                                          line + (lx - x));
            }
        }

        heap_caps_free(line);
        free(rgba);
        return true;
    }

    // ── Draw PNG scaled to fit a box ────────────────────────────────────

    bool DisplayService::drawPngScaled(int x, int y, int destW, int destH,
                                       const uint8_t* data, size_t size)
    {
        if (!panelHandle_ || !data || size == 0) return false;
        auto* panel = static_cast<esp_lcd_panel_handle_t>(panelHandle_);

        uint8_t* rgba = nullptr;
        uint32_t pw = 0, ph = 0;
        int err = png_decode_rgba(data, size, &rgba, &pw, &ph);
        if (err != 0 || !rgba)
        {
            ESP_LOGE(TAG, "PNG decode failed: %d", err);
            return false;
        }

        if (destW <= 0) destW = width_;
        if (destH <= 0) destH = height_;

        // Fit into the box preserving aspect ratio
        int dw = destW;
        int dh = destH;
        if (pw > 0 && ph > 0)
        {
            dw = static_cast<int>(static_cast<int64_t>(pw) * destH / ph);
            dh = destH;
            if (dw > destW)
            {
                dw = destW;
                dh = static_cast<int>(static_cast<int64_t>(ph) * destW / pw);
            }
        }
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;

        // Centre inside the box
        const int ox = x + (destW - dw) / 2;
        const int oy = y + (destH - dh) / 2;

        ESP_LOGI(TAG, "PNG %ux%u scaled to %dx%d, drawing at (%d,%d)",
                 pw, ph, dw, dh, ox, oy);

        uint16_t* line = static_cast<uint16_t*>(
            heap_caps_malloc(dw * sizeof(uint16_t), MALLOC_CAP_DMA));
        if (!line)
        {
            free(rgba);
            return false;
        }

        // Bilinear resample row-by-row, alpha-blend over white, then
        // pack to RGB565 with a Bayer dither to fight 5/6/5 banding.
        for (int dy = 0; dy < dh; ++dy)
        {
            const int ly = oy + dy;
            if (ly < 0 || ly >= height_) continue;

            // Map dest row to source row: integer index + 16-bit fraction
            const uint32_t sy_num = static_cast<uint32_t>(dy) * ph;
            const uint32_t sy0_raw = sy_num / dh;
            const uint32_t fy = ((sy_num % dh) << 16) / dh;     // 0..65535
            const uint32_t ify = 65536 - fy;
            const uint32_t sy0 = (sy0_raw < ph) ? sy0_raw : ph - 1;
            const uint32_t sy1 = (sy0 + 1 < ph) ? sy0 + 1 : ph - 1;
            const uint8_t* row0 = rgba + static_cast<size_t>(sy0) * pw * 4;
            const uint8_t* row1 = rgba + static_cast<size_t>(sy1) * pw * 4;

            for (int dx = 0; dx < dw; ++dx)
            {
                const uint32_t sx_num = static_cast<uint32_t>(dx) * pw;
                const uint32_t sx0_raw = sx_num / dw;
                const uint32_t fx = ((sx_num % dw) << 16) / dw;
                const uint32_t ifx = 65536 - fx;
                const uint32_t sx0 = (sx0_raw < pw) ? sx0_raw : pw - 1;
                const uint32_t sx1 = (sx0 + 1 < pw) ? sx0 + 1 : pw - 1;

                const uint8_t* p00 = row0 + sx0 * 4;
                const uint8_t* p10 = row0 + sx1 * 4;
                const uint8_t* p01 = row1 + sx0 * 4;
                const uint8_t* p11 = row1 + sx1 * 4;

                // Bilinear blend each 8-bit channel (including alpha)
                auto bilerp = [&] (int ch) -> int {
                    const int v0 = (p00[ch] * ifx + p10[ch] * fx) >> 16;
                    const int v1 = (p01[ch] * ifx + p11[ch] * fx) >> 16;
                    return (v0 * ify + v1 * fy) >> 16;
                };
                int r = bilerp(0);
                int g = bilerp(1);
                int b = bilerp(2);
                int a = bilerp(3);

                // Alpha-blend over white background
                if (a < 255)
                {
                    r = (r * a + 255 * (255 - a)) / 255;
                    g = (g * a + 255 * (255 - a)) / 255;
                    b = (b * a + 255 * (255 - a)) / 255;
                }

                line[dx] = rgb565Dithered(static_cast<uint8_t>(r),
                                          static_cast<uint8_t>(g),
                                          static_cast<uint8_t>(b),
                                          dx, dy);
            }

            int lx = ox;
            int x2 = lx + dw;
            if (x2 > width_) x2 = width_;
            if (lx < 0) lx = 0;
            if (lx < x2)
            {
                esp_lcd_panel_draw_bitmap(panel, lx, ly, x2, ly + 1,
                                          line + (lx - ox));
            }
        }

        heap_caps_free(line);
        free(rgba);
        return true;
    }

    // ── Blit pre-formatted RGB565 image ──────────────────────────────────

    void DisplayService::drawRgb565(int x, int y, int w, int h,
                                    const uint16_t* rgb565)
    {
        if (!panelHandle_ || !rgb565) return;
        auto* panel = static_cast<esp_lcd_panel_handle_t>(panelHandle_);

        // Clip to screen bounds
        int x1 = x, y1 = y, x2 = x + w, y2 = y + h;
        if (x1 < 0) x1 = 0;
        if (y1 < 0) y1 = 0;
        if (x2 > width_)  x2 = width_;
        if (y2 > height_) y2 = height_;
        if (x1 >= x2 || y1 >= y2) return;

        // SPI DMA needs DMA-capable memory: copy each row from the
        // (flash-mapped) source into internal RAM before pushing it.
        uint16_t* line = static_cast<uint16_t*>(
            heap_caps_malloc(w * sizeof(uint16_t), MALLOC_CAP_DMA));
        if (!line) return;

        for (int row = y1; row < y2; ++row)
        {
            std::memcpy(line, rgb565 + (row - y) * w,
                        w * sizeof(uint16_t));
            esp_lcd_panel_draw_bitmap(panel, x1, row, x2, row + 1,
                                      line + (x1 - x));
        }

        heap_caps_free(line);
    }

    // ── GIF animation ──────────────────────────────────────────────────────

    bool DisplayService::playGif(int x, int y,
                                  const uint8_t* data, size_t size)
    {
        if (!panelHandle_) return false;
        return gifAnim_.play(panelHandle_, x, y, data, size);
    }

    void DisplayService::stopGif()
    {
        gifAnim_.stop();
    }

    // ── Asset lookup (single public rendering entry point) ─────────────────

    bool DisplayService::showAsset(const char* name)
    {
        if (!name) return false;
        // Stop any running animation so a follow-up static blit isn't
        // immediately overwritten by the next decoded GIF frame.
        gifAnim_.stop();

        char buf[64];

        // Try "<name>.png" first — embedded raw, decoded at runtime
        // by the built-in PNG decoder, scaled to fit the panel with
        // bilinear resampling, alpha-blended over white.
        snprintf(buf, sizeof(buf), "%s.png", name);
        if (const ImageAsset* img = findImage(buf))
        {
            fillColour(rgb565(255, 255, 255));
            drawPngScaled(0, 0, width_, height_, img->data, img->size);
            return true;
        }

        // Then "<name>.gif" (animated) as a last-resort fallback.
        snprintf(buf, sizeof(buf), "%s.gif", name);
        if (const ImageAsset* gif = findImage(buf))
        {
            fillColour(rgb565(255, 255, 255));
            playGif(0, 0, gif->data, gif->size);
            return true;
        }

        ESP_LOGW(TAG, "no asset for '%s' (looked for %s.png and %s.gif)",
                 name, name, name);
        return false;
    }

    // ── Service lifecycle ──────────────────────────────────────────────────

    void DisplayService::start()
    {
        ESP_LOGI(TAG, "display starting...");
        if (!initPanel()) {
            ESP_LOGE(TAG, "initPanel failed!");
            return;
        }
        // Panel is left filled with white (from initPanel) so it never
        // boots with random garbage. The application is responsible
        // for choosing what to render via showAsset().
        ESP_LOGI(TAG, "display started — %dx%d", width_, height_);
    }

    void DisplayService::stop()
    {
        if (panelHandle_)
        {
            gifAnim_.stop();

            auto* panel = static_cast<esp_lcd_panel_handle_t>(panelHandle_);

            if (kBlk >= 0)
            {
                gpio_set_level(static_cast<gpio_num_t>(kBlk), 0);
            }

            esp_lcd_panel_disp_on_off(panel, false);
            esp_lcd_panel_del(panel);
            panelHandle_ = nullptr;
            ESP_LOGI(TAG, "display stopped");
        }
    }
} // namespace embed::display
