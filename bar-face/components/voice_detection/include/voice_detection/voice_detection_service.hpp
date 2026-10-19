#pragma once

#include "embed/embed.hpp"
#include "embed_core/wifi_service.hpp"
#include <cstdint>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace embed::audio {

// ── Messages ────────────────────────────────────────────────────────────

/// Emitted when the wake-word engine fires (WakeNet or MultiNet).
struct WakeWordDetected {
    uint32_t wakeWordId;    ///< WakeNet command-id (≥ 1) or 200+ for MultiNet
    int32_t  threshold;     ///< detection threshold that was used
};
static_assert(embed::Message<WakeWordDetected>);

/// Emitted on every VAD state change.
struct VoiceActivityEvent {
    /// 0 = silence, 1 = speech (matches esp-sr vad_state_t values).
    uint8_t  vadState;
    uint32_t frameIndex;    ///< monotonic fetch-loop counter
};
static_assert(embed::Message<VoiceActivityEvent>);

// ── Service ─────────────────────────────────────────────────────────────

/// Direct callback for raw PCM audio frames read from the microphone.
/// Fires in the feed task context (core 1) after each audioBus->record() call.
/// @param samples  pointer to PCM 16-bit mono samples
/// @param count    number of samples (not bytes)
/// @param ctx      user context pointer
using AudioFrameCallback = void(*)(const int16_t* samples, size_t count,
                                   void* ctx);

/// Audio front-end + voice detection via esp-sr.
///
/// Owns the full voice pipeline:
///   • **feed**  – reads audio from AudioBusService (mic), pushes into AFE + MultiNet
///   • **fetch** – pulls AFE results, emits VAD / WakeNet events
///
/// Wake word detection uses two engines:
///   • WakeNet (via AFE) — pre-built models (Hi Jason, Alexa, …)
///   • MultiNet (command-word recognizer) — configurable commands
class VoiceDetectionService : public embed::Service {
public:
    VoiceDetectionService() = default;
    const char* serviceName() const override { return "VoiceDetectionService"; }

    void start() override;
    void stop() override;

    /// Wake-word detected (WakeNet or custom).
    Signal<WakeWordDetected>    onWakeWord;
    /// VAD state changed.
    Signal<VoiceActivityEvent>  onVoiceActivity;

    /// Register a callback that fires for every raw mic audio frame
    /// read by the feed task. Multiple callbacks are supported (up to 4).
    /// Pass nullptr to unregister; context is used to match the entry.
    void setAudioFrameCallback(AudioFrameCallback cb, void* ctx);

    /// Enable/disable voice processing (mic → AFE → callback + VAD events).
    /// When disabled, feed task still reads I2S (to keep mic hardware happy)
    /// but does NOT call the audio frame callback or emit VAD events.
    /// This prevents speaker → mic feedback during TTS playback.
    void EnableVoiceProcessing(bool enable);

    /// Enable/disable wake word detection independently.
    /// Wake word detection is only meaningful when voice processing is enabled.
    void EnableWakeWordDetection(bool enable);

private:
    // ── task entry points ──
    static void feedTaskEntry(void* arg);
    static void fetchTaskEntry(void* arg);

    // ── Signal/Slot wiring ──
    Slot<WifiConnected>    wifiConnectedSlot_{onWifiConnected, this};
    Slot<WifiDisconnected> wifiDisconnectedSlot_{onWifiDisconnected, this};
    static void onWifiConnected(const WifiConnected& msg, void* ctx);
    static void onWifiDisconnected(const WifiDisconnected& msg, void* ctx);

    // ── state ──
    /// Opaque AFE types — forward-declared to keep esp-sr out of the header.
    const void* afeHandle_ = nullptr;   ///< esp_afe_sr_iface_t*
    void*       afeData_   = nullptr;   ///< esp_afe_sr_data_t*

    TaskHandle_t feedTask_  = nullptr;
    TaskHandle_t fetchTask_ = nullptr;

    volatile bool running_       = false;
    bool          hasWakeNet_    = false;
    uint32_t      frameIndex_    = 0;

    // Voice processing pause (xiaozhi-style: mic reads continue but no callback/VAD)
    volatile bool voiceProcessingEnabled_ = true;
    bool          wakeWordEnabled_        = true;

    // Audio frame callbacks (multiple listeners supported)
    static constexpr int kMaxAudioFrameCallbacks = 4;
    struct AudioCbEntry {
        AudioFrameCallback cb  = nullptr;
        void*              ctx = nullptr;
    };
    AudioCbEntry audioFrameCbs_[kMaxAudioFrameCallbacks];

    // ── MultiNet (ESP-SR command-word recognizer) ──
    const void*          mnHandle_     = nullptr;  ///< esp_mn_iface_t*
    void*                mnModelData_  = nullptr;  ///< model_iface_data_t*
    bool                 hasMultiNet_  = false;
    int                  mnChunkSize_  = 0;
    std::vector<int16_t> mnBuffer_;                ///< PCM accumulation buffer
};

} // namespace embed::audio
