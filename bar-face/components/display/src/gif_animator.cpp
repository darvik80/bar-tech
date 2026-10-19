#include "display/gif_animator.hpp"
#include "decoders/gif_decoder.hpp"

#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>

static constexpr char TAG_GIFA[] = "GifAnim";

namespace embed::display {

// ── RGB888 → RGB565 ────────────────────────────────────────────────────

static inline uint16_t to_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(
        ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// ── Destructor ─────────────────────────────────────────────────────────

GifAnimator::~GifAnimator() {
    stop();
}

// ── play() ─────────────────────────────────────────────────────────────

bool GifAnimator::play(void* panel, int x, int y,
                       const uint8_t* data, size_t size)
{
    // Stop any running animation first
    stop();

    if (!panel || !data || size == 0) return false;

    // Open the GIF
    GifHandle* g = gif_open(data, size);
    if (!g) {
        ESP_LOGE(TAG_GIFA, "gif_open failed");
        return false;
    }

    const auto& info = gif_info(g);

    // Allocate DMA-capable line buffer (RGB565)
    uint16_t* lb = static_cast<uint16_t*>(
        heap_caps_malloc(info.canvas_w * sizeof(uint16_t), MALLOC_CAP_DMA));
    if (!lb) {
        ESP_LOGE(TAG_GIFA, "line buffer alloc failed");
        gif_close(g);
        return false;
    }

    panel_   = panel;
    drawX_   = x;
    drawY_   = y;
    gif_     = g;
    lineBuf_ = lb;
    running_ = true;

    // Create animation task. Stack size: the LZW decoder state is
    // held in the GifHandle on the heap (see gif_decoder.cpp), so the
    // per-frame stack budget is just function-call overhead plus a
    // small margin. 8 KB is a healthy safety net for the decoder
    // helpers, FreeRTOS internals, and the SPI panel draw path.
    BaseType_t ret = xTaskCreatePinnedToCore(
        animTaskEntry, "gif_anim", 8192, this, 5, &taskHandle_, 1);

    if (ret != pdPASS) {
        ESP_LOGE(TAG_GIFA, "task create failed");
        running_ = false;
        gif_close(g);
        heap_caps_free(lb);
        gif_ = nullptr;
        lineBuf_ = nullptr;
        return false;
    }

    ESP_LOGI(TAG_GIFA, "GIF playback started: %ux%u at (%d,%d)",
             info.canvas_w, info.canvas_h, x, y);
    return true;
}

// ── stop() ─────────────────────────────────────────────────────────────

void GifAnimator::stop() {
    if (!running_) return;

    running_ = false;

    // Wait for task to finish (it checks running_ each iteration)
    if (taskHandle_) {
        // Give the task time to notice running_ == false and exit
        vTaskDelay(pdMS_TO_TICKS(200));
        // Delete if still alive
        eTaskState state = eTaskGetState(taskHandle_);
        if (state != eDeleted) {
            vTaskDelete(taskHandle_);
        }
        taskHandle_ = nullptr;
    }

    if (gif_) {
        gif_close(gif_);
        gif_ = nullptr;
    }
    if (lineBuf_) {
        heap_caps_free(lineBuf_);
        lineBuf_ = nullptr;
    }

    ESP_LOGI(TAG_GIFA, "GIF playback stopped");
}

// ── FreeRTOS task entry ────────────────────────────────────────────────

void GifAnimator::animTaskEntry(void* arg) {
    auto* self = static_cast<GifAnimator*>(arg);
    self->animTask();
    self->taskHandle_ = nullptr;
    vTaskDelete(nullptr);
}

// ── Animation loop ─────────────────────────────────────────────────────

void GifAnimator::animTask() {
    auto* panel = static_cast<esp_lcd_panel_handle_t>(panel_);
    const auto& info = gif_info(gif_);
    const int cw = info.canvas_w;
    const int ch = info.canvas_h;

    while (running_) {
        int ret = gif_next_frame(gif_);

        if (ret == 0) {
            // No more frames — rewind and loop
            gif_rewind(gif_);
            ret = gif_next_frame(gif_);
            if (ret == 0) {
                ESP_LOGW(TAG_GIFA, "empty GIF after rewind");
                break;
            }
        }

        const auto& fr = gif_frame(gif_);
        const uint8_t* canvas = gif_canvas(gif_);

        // Draw canvas to LCD line-by-line
        for (int row = 0; row < ch && running_; ++row) {
            const uint8_t* src = canvas + row * cw * 3;
            for (int col = 0; col < cw; ++col) {
                lineBuf_[col] = to_rgb565(src[col * 3],
                                           src[col * 3 + 1],
                                           src[col * 3 + 2]);
            }
            int ly = drawY_ + row;
            esp_lcd_panel_draw_bitmap(panel,
                                      drawX_, ly,
                                      drawX_ + cw, ly + 1,
                                      lineBuf_);
        }

        // Wait for frame delay (convert centiseconds → ticks)
        uint16_t delay_cs = fr.delay_cs;
        if (delay_cs < 2) delay_cs = 10; // minimum 20 ms to avoid busy-loop
        TickType_t ticks = pdMS_TO_TICKS(delay_cs * 10);
        if (ticks == 0) ticks = 1;
        vTaskDelay(ticks);
    }

    running_ = false;
    ESP_LOGI(TAG_GIFA, "animation task finished");
}

} // namespace embed::display
