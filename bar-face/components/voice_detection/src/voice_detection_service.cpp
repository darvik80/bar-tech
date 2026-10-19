#include "voice_detection/voice_detection_service.hpp"
#include "audio_bus/audio_bus_service.hpp"

#include "esp_afe_sr_iface.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_models.h"
#include "esp_wn_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"

#include "embed/registry.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <cstdlib>
#include <cstring>

static constexpr char TAG[] = "VoiceDet";

static constexpr int kSampleRate = 16000;

// ── Task context passed between service and FreeRTOS tasks ─────────────

namespace embed::audio {

namespace {

struct TaskCtx {
    VoiceDetectionService* self;
    const esp_afe_sr_iface_t*     afeHandle;
    esp_afe_sr_data_t*            afeData;
    AudioBusService*              audioBus;
    int                           feedChunkSize;
    int                           feedNch;
};

} // namespace

// ── Feed task: AudioBus (mic) → AFE + audio callbacks ──────────────────

void VoiceDetectionService::feedTaskEntry(void* arg) {
    auto* ctx = static_cast<TaskCtx*>(arg);
    const int bufBytes = ctx->feedChunkSize * ctx->feedNch * static_cast<int>(sizeof(int16_t));
    auto* buff = static_cast<int16_t*>(malloc(bufBytes));
    if (!buff) {
        ESP_LOGE(TAG, "feed buffer alloc failed (%d bytes)", bufBytes);
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "feed task started (chunk=%d ch=%d)", ctx->feedChunkSize, ctx->feedNch);

    while (ctx->self->running_) {
        size_t samplesRead = ctx->audioBus->record(
            buff, static_cast<size_t>(ctx->feedChunkSize * ctx->feedNch));
        if (samplesRead == 0) {
            continue;
        }
        ctx->afeHandle->feed(ctx->afeData, buff);

        // Forward raw PCM to all registered callbacks + MultiNet
        // (only when voice processing is enabled — prevents speaker→mic feedback)
        if (ctx->self->voiceProcessingEnabled_) {
            // ── MultiNet command-word detection ──
            if (ctx->self->wakeWordEnabled_ && ctx->self->hasMultiNet_) {
                ctx->self->mnBuffer_.insert(
                    ctx->self->mnBuffer_.end(), buff, buff + samplesRead);
                auto* mnIface = static_cast<const esp_mn_iface_t*>(ctx->self->mnHandle_);
                while (static_cast<int>(ctx->self->mnBuffer_.size()) >= ctx->self->mnChunkSize_) {
                    esp_mn_state_t mnState = mnIface->detect(
                        static_cast<model_iface_data_t*>(ctx->self->mnModelData_),
                        ctx->self->mnBuffer_.data());
                    if (mnState == ESP_MN_STATE_DETECTED) {
                        esp_mn_results_t* res = mnIface->get_results(
                            static_cast<model_iface_data_t*>(ctx->self->mnModelData_));
                        for (int i = 0; i < res->num; i++) {
                            ESP_LOGI(TAG, "MultiNet detected: cmd=%d str='%s' prob=%.2f",
                                     res->command_id[i], res->string, res->prob[i]);
                            WakeWordDetected msg{};
                            msg.wakeWordId = 200 + res->command_id[i];
                            msg.threshold  = static_cast<int32_t>(res->prob[i] * 100);
                            ctx->self->onWakeWord.emit(msg);
                        }
                        mnIface->clean(static_cast<model_iface_data_t*>(ctx->self->mnModelData_));
                    } else if (mnState == ESP_MN_STATE_TIMEOUT) {
                        mnIface->clean(static_cast<model_iface_data_t*>(ctx->self->mnModelData_));
                    }
                    ctx->self->mnBuffer_.erase(
                        ctx->self->mnBuffer_.begin(),
                        ctx->self->mnBuffer_.begin() + ctx->self->mnChunkSize_);
                }
            }

            for (int i = 0; i < VoiceDetectionService::kMaxAudioFrameCallbacks; i++) {
                auto& entry = ctx->self->audioFrameCbs_[i];
                if (entry.cb) {
                    entry.cb(buff, samplesRead, entry.ctx);
                }
            }
        }
    }

    free(buff);
    ESP_LOGI(TAG, "feed task stopped");
    vTaskDelete(nullptr);
}

// ── Fetch task: AFE → Signal emit ──────────────────────────────────────

void VoiceDetectionService::fetchTaskEntry(void* arg) {
    auto* ctx = static_cast<TaskCtx*>(arg);

    ESP_LOGI(TAG, "fetch task started");

    while (ctx->self->running_) {
        afe_fetch_result_t* res = ctx->afeHandle->fetch(ctx->afeData);
        if (!res || res->ret_value == ESP_FAIL) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Skip all event emission when voice processing is disabled
        // (TTS playback in progress — speaker → mic feedback prevention)
        if (!ctx->self->voiceProcessingEnabled_) {
            continue;
        }

        ctx->self->frameIndex_++;

        // ── Wake-word trigger (WakeNet) ──
        if (ctx->self->wakeWordEnabled_ && ctx->self->hasWakeNet_ && res->wakeup_state == WAKENET_DETECTED) {
            WakeWordDetected msg{};
            msg.wakeWordId = res->wakenet_model_index;
            msg.threshold  = 0;
            ctx->self->onWakeWord.emit(msg);
            ESP_LOGI(TAG, "WAKE WORD DETECTED cmd_id=%u", msg.wakeWordId);
        }

        // ── VAD state change ──
        VoiceActivityEvent vadMsg{};
        vadMsg.vadState    = static_cast<uint8_t>(res->vad_state);
        vadMsg.frameIndex  = ctx->self->frameIndex_;
        ctx->self->onVoiceActivity.emit(vadMsg);
    }

    ESP_LOGI(TAG, "fetch task stopped");
    vTaskDelete(nullptr);
}

// ── setAudioFrameCallback ──────────────────────────────────────────────

void VoiceDetectionService::setAudioFrameCallback(AudioFrameCallback cb,
                                                   void* ctx) {
    if (!cb) {
        // Clear all callbacks matching this context (nullptr clears all)
        for (auto& entry : audioFrameCbs_) {
            if (ctx == nullptr || entry.ctx == ctx) {
                entry.cb  = nullptr;
                entry.ctx = nullptr;
            }
        }
        ESP_LOGI(TAG, "audio frame callback %s", ctx ? "unregistered" : "cleared");
        return;
    }
    // Update existing entry with same context, or add new one
    int freeSlot = -1;
    for (int i = 0; i < kMaxAudioFrameCallbacks; i++) {
        if (audioFrameCbs_[i].ctx == ctx && audioFrameCbs_[i].cb) {
            audioFrameCbs_[i].cb = cb;
            ESP_LOGI(TAG, "audio frame callback updated (slot %d)", i);
            return;
        }
        if (freeSlot < 0 && !audioFrameCbs_[i].cb) freeSlot = i;
    }
    if (freeSlot >= 0) {
        audioFrameCbs_[freeSlot].cb  = cb;
        audioFrameCbs_[freeSlot].ctx = ctx;
        ESP_LOGI(TAG, "audio frame callback registered (slot %d)", freeSlot);
    } else {
        ESP_LOGE(TAG, "audio frame callback slots full (%d)", kMaxAudioFrameCallbacks);
    }
}

// ── EnableVoiceProcessing ──────────────────────────────────────────────

void VoiceDetectionService::EnableVoiceProcessing(bool enable) {
    voiceProcessingEnabled_ = enable;
    if (enable && afeHandle_ && afeData_) {
        // Reset VAD state to clear any residual speech detection from speaker tail audio.
        // Without this, re-enabling voice processing after TTS playback can trigger
        // false START_OF_SPEECH from the speaker's physical settling time.
        auto* handle = static_cast<const esp_afe_sr_iface_t*>(afeHandle_);
        if (handle->reset_vad) {
            handle->reset_vad(static_cast<esp_afe_sr_data_t*>(afeData_));
            ESP_LOGI(TAG, "VAD reset on re-enable (clear residual speech detection)");
        }
    }
    ESP_LOGI(TAG, "voice processing %s", enable ? "ENABLED" : "DISABLED");
}

// ── EnableWakeWordDetection ────────────────────────────────────────────

void VoiceDetectionService::EnableWakeWordDetection(bool enable) {
    wakeWordEnabled_ = enable;
    ESP_LOGI(TAG, "wake word detection %s", enable ? "ENABLED" : "DISABLED");
}

// ── start() ────────────────────────────────────────────────────────────

void VoiceDetectionService::start() {
    // ── Connect to WiFi events ──
    auto* wifi = embed::ServiceRegistry::instance().getService<embed::WifiService>();
    if (wifi) {
        wifiConnectedSlot_.connect(wifi->onConnected);
        wifiDisconnectedSlot_.connect(wifi->onDisconnected);
    }

    // ── Check AudioBusService availability ──
    auto* audioBus = embed::ServiceRegistry::instance().getService<embed::audio::AudioBusService>();
    if (!audioBus || !audioBus->isReady()) {
        ESP_LOGE(TAG, "AudioBusService not available — voice detection disabled");
        return;
    }

    // ── Discover compiled-in speech models ──
    srmodel_list_t* models = esp_srmodel_init("model");
    if (!models) {
        ESP_LOGE(TAG, "esp_srmodel_init returned NULL — "
                       "select a WakeNet model in menuconfig: "
                       "Component config → ESP-SR → WakeNet");
        return;
    }

    // ── AFE configuration (single-mic, speech-recognition pipeline) ──
    afe_config_t* afeConfig = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (!afeConfig) {
        ESP_LOGE(TAG, "afe_config_init failed");
        return;
    }

    hasWakeNet_ = afeConfig->wakenet_init && afeConfig->wakenet_model_name != nullptr;
    if (!hasWakeNet_) {
        ESP_LOGW(TAG, "no WakeNet model compiled — VAD only");
    } else {
        ESP_LOGI(TAG, "WakeNet model: %s", afeConfig->wakenet_model_name);
    }

    // ── Create AFE instance ──
    auto* handle = esp_afe_handle_from_config(afeConfig);
    auto* data   = handle->create_from_config(afeConfig);
    afeHandle_   = handle;
    afeData_     = data;

    const int feedChunkSize = handle->get_feed_chunksize(data);
    const int feedNch       = handle->get_feed_channel_num(data);

    // ── Launch tasks ──
    running_ = true;

    static TaskCtx ctx;
    ctx.self          = this;
    ctx.afeHandle     = handle;
    ctx.afeData       = data;
    ctx.audioBus      = audioBus;
    ctx.feedChunkSize = feedChunkSize;
    ctx.feedNch       = feedNch;

    xTaskCreatePinnedToCore(feedTaskEntry, "vd_feed",
                            CONFIG_EMBED_VOICE_DETECTION_FEED_TASK_STACK,
                            &ctx, 5, &feedTask_, 1);
    if (!feedTask_) {
        ESP_LOGE(TAG, "feed task create failed");
        stop();
        return;
    }

    xTaskCreatePinnedToCore(fetchTaskEntry, "vd_fetch",
                            CONFIG_EMBED_VOICE_DETECTION_FETCH_TASK_STACK,
                            &ctx, 5, &fetchTask_, 0);
    if (!fetchTask_) {
        ESP_LOGE(TAG, "fetch task create failed");
        stop();
        return;
    }

    // ── MultiNet (command-word recognizer) ──
    char* mnName = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_ENGLISH);
    if (mnName) {
        auto* mnIface = esp_mn_handle_from_name(mnName);
        if (mnIface) {
            mnHandle_    = mnIface;
            mnModelData_ = mnIface->create(mnName, 3000);
            if (mnModelData_) {
                float threshold = CONFIG_EMBED_VOICE_DETECTION_MN_THRESHOLD / 100.0f;
                mnIface->set_det_threshold(static_cast<model_iface_data_t*>(mnModelData_), threshold);
                mnChunkSize_ = mnIface->get_samp_chunksize(static_cast<model_iface_data_t*>(mnModelData_));

                // Register command words from Kconfig
                esp_mn_commands_clear();
                esp_mn_commands_add(1, CONFIG_EMBED_VOICE_DETECTION_MN_COMMAND);
                esp_mn_commands_update();
                mnIface->print_active_speech_commands(static_cast<model_iface_data_t*>(mnModelData_));

                hasMultiNet_ = true;
                mnBuffer_.reserve(mnChunkSize_ * 2);
                ESP_LOGI(TAG, "MultiNet model: %s chunk=%d", mnName, mnChunkSize_);
            }
        }
    }
    if (!hasMultiNet_) {
        ESP_LOGW(TAG, "MultiNet not available — command-word detection disabled");
    }

    afe_config_free(afeConfig);

    ESP_LOGI(TAG, "started — WakeNet=%s MultiNet=%s feed_chunk=%d ch=%d",
             hasWakeNet_ ? "ON" : "OFF",
             hasMultiNet_ ? "ON" : "OFF",
             feedChunkSize, feedNch);
}

// ── stop() ─────────────────────────────────────────────────────────────

void VoiceDetectionService::stop() {
    running_ = false;

    // Give tasks time to exit their loops.
    vTaskDelay(pdMS_TO_TICKS(200));

    if (feedTask_) {
        vTaskDelete(feedTask_);
        feedTask_ = nullptr;
    }
    if (fetchTask_) {
        vTaskDelete(fetchTask_);
        fetchTask_ = nullptr;
    }

    if (afeData_ && afeHandle_) {
        static_cast<const esp_afe_sr_iface_t*>(afeHandle_)->destroy(
            static_cast<esp_afe_sr_data_t*>(afeData_));
        afeHandle_ = nullptr;
        afeData_   = nullptr;
    }

    if (hasMultiNet_ && mnHandle_ && mnModelData_) {
        static_cast<const esp_mn_iface_t*>(mnHandle_)->destroy(
            static_cast<model_iface_data_t*>(mnModelData_));
        mnHandle_    = nullptr;
        mnModelData_ = nullptr;
    }
    hasMultiNet_ = false;
    mnBuffer_.clear();

    hasWakeNet_ = false;
    frameIndex_ = 0;
    ESP_LOGI(TAG, "stopped");
}

// ── WiFi event handlers ────────────────────────────────────────────────

void VoiceDetectionService::onWifiConnected(
    const embed::WifiConnected& /*msg*/, void* ctx)
{
    auto* self = static_cast<VoiceDetectionService*>(ctx);
    if (!self->running_) {
        ESP_LOGI(TAG, "WiFi up — voice detection active");
    }
}

void VoiceDetectionService::onWifiDisconnected(
    const embed::WifiDisconnected& /*msg*/, void* ctx)
{
    auto* self = static_cast<VoiceDetectionService*>(ctx);
    if (self->running_) {
        ESP_LOGI(TAG, "WiFi down — voice detection continues (local only)");
    }
}

} // namespace embed::audio
