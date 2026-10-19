#pragma once

#include "embed/service.hpp"
#include "display/gif_animator.hpp"

#include <cstdint>
#include <cstddef>

namespace embed::display {

/// ST7789 SPI LCD display service.
///
/// Pure rendering service — no event subscriptions, no timers, no
/// coupling to voice detection or any other service. start() inits
/// the panel via esp_lcd and leaves it white; the application is
/// responsible for choosing what to render via showAsset().
///
/// The asset table comes from assets/ (see
/// embedded_images.h). showAsset() probes "<name>.png" first
/// (decoded at runtime by the built-in PNG decoder, scaled to fit
/// the panel with bilinear resampling) and falls back to
/// "<name>.gif" (animated, played at runtime by GifAnimator).
class DisplayService : public embed::Service {
public:
    DisplayService() = default;
    const char* serviceName() const override { return "DisplayService"; }

    void start() override;
    void stop() override;

    /// Try to show an asset by base name (e.g. "barmen-listen").
    /// Stops any running animation first, then probes "<name>.png"
    /// (decoded at runtime by the built-in PNG decoder, scaled to
    /// fit the panel) and "<name>.gif" (animated, run through
    /// GifAnimator). Returns true when something was drawn.
    /// Logs a warning and leaves the panel unchanged when neither
    /// is embedded. Thread-safe — can be called from any task.
    bool showAsset(const char* name);

    /// Fill the entire screen with a 16-bit RGB565 colour.
    void fillColour(uint16_t rgb565);

    /// Draw a text string at (x, y) using the built-in 8×8 bitmap font.
    /// Colour is 16-bit RGB565.
    void drawText(int x, int y, const char* text, uint16_t colour);

    /// Draw raw RGB565 bitmap at (x, y). Low-level primitive.
    void drawBitmap(int x, int y, int w, int h, const uint16_t* rgb565);

    /// Decode PNG from memory buffer and draw at (x, y).
    /// Data can come from flash, FS, or network.
    /// Returns true on success.
    bool drawPng(int x, int y, const uint8_t* data, size_t size);

    /// Decode PNG from memory buffer, scale it to fit the destW x destH
    /// box preserving aspect ratio (centred inside the box), and draw.
    /// Alpha is blended against white. Returns true on success.
    bool drawPngScaled(int x, int y, int destW, int destH,
                       const uint8_t* data, size_t size);

    /// Blit a pre-formatted RGB565 image buffer at (x, y) of size w x h.
    /// The buffer must be a contiguous w*h array of uint16_t pixels in
    /// RGB565 LE format. No decode, no scaling, no dithering — a
    /// row-by-row copy straight to the panel.
    void drawRgb565(int x, int y, int w, int h, const uint16_t* rgb565);

    /// Start playing an animated GIF at (x, y).
    /// The data buffer must remain valid for the lifetime of playback.
    /// Stops any previously running GIF animation.
    bool playGif(int x, int y, const uint8_t* data, size_t size);

    /// Stop current GIF animation.
    void stopGif();

private:
    bool initPanel();

    void* panelHandle_ = nullptr;   ///< esp_lcd_panel_handle_t
    int width_  = 0;
    int height_ = 0;
    GifAnimator gifAnim_;
};

} // namespace embed::display
