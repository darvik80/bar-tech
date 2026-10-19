#pragma once

#include <cstdint>
#include <cstddef>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace embed::display {

struct GifHandle;

/// Animated GIF player — decodes frames in a FreeRTOS task and
/// pushes them to the LCD panel via esp_lcd_panel_draw_bitmap().
class GifAnimator {
public:
    GifAnimator() = default;
    ~GifAnimator();

    // Non-copyable
    GifAnimator(const GifAnimator&) = delete;
    GifAnimator& operator=(const GifAnimator&) = delete;

    /// Start playing an animated GIF from a memory buffer.
    /// The buffer must remain valid for the lifetime of playback.
    /// Stops any previously running animation.
    /// @param panel  esp_lcd_panel_handle_t (void* to hide the type)
    /// @param x, y   top-left position on the LCD
    bool play(void* panel, int x, int y,
              const uint8_t* data, size_t size);

    /// Stop current animation and free resources.
    void stop();

    bool isPlaying() const { return running_; }

private:
    static void animTaskEntry(void* arg);
    void animTask();

    void*       panel_       = nullptr;
    int         drawX_       = 0;
    int         drawY_       = 0;
    TaskHandle_t taskHandle_ = nullptr;
    GifHandle*  gif_         = nullptr;
    uint16_t*   lineBuf_     = nullptr;   ///< DMA-capable RGB565 line buffer
    volatile bool running_   = false;
};

} // namespace embed::display
