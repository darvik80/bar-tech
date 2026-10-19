#pragma once

#include "embed/embed.hpp"
#include "embed_core/wifi_service.hpp"
#include "embed_core/metrics_service.hpp"
#include "embed_core/mqtt_service.hpp"
#include "voice_detection/voice_detection_service.hpp"

#include "esp_log.h"

/// Monitors and logs key system events: WiFi, MQTT, metrics, voice activity.
class MonitorService : public embed::Service {
public:
    const char* serviceName() const override { return "MonitorService"; }

    void start() override {
        auto& reg = embed::ServiceRegistry::instance();
        auto* wifi    = reg.getService<embed::WifiService>();
        auto* metrics = reg.getService<embed::MetricsService>();
        auto* mqtt    = reg.getService<embed::MqttService>();
        auto* voice   = reg.getService<embed::audio::VoiceDetectionService>();

        if (wifi) {
            wifi_connected_slot_.connect(wifi->onConnected);
            wifi_disconnected_slot_.connect(wifi->onDisconnected);
        }
        if (metrics) metrics_slot_.connect(metrics->onMetricsCollected);
        if (mqtt) {
            mqtt_connected_slot_.connect(mqtt->onConnected);
            mqtt_disconnected_slot_.connect(mqtt->onDisconnected);
            mqtt_message_slot_.connect(mqtt->onMessage);
        }
        if (voice) {
            wake_word_slot_.connect(voice->onWakeWord);
            voice_activity_slot_.connect(voice->onVoiceActivity);
        }
        ESP_LOGI("Monitor", "slots connected: wifi=%d metrics=%d mqtt=%d voice=%d",
                 wifi != nullptr, metrics != nullptr, mqtt != nullptr, voice != nullptr);
    }

private:
    static void onWifiConnected(const embed::WifiConnected& msg, void* /*ctx*/) {
        ESP_LOGI("Monitor", "WiFi CONNECTED, IP: %s", msg.ip.c_str());
    }

    static void onWifiDisconnected(const embed::WifiDisconnected& msg, void* /*ctx*/) {
        ESP_LOGI("Monitor", "WiFi DISCONNECTED, reason=%u", msg.reason);
    }

    static void onMetricsCollected(const embed::MetricsCollected& msg, void* /*ctx*/) {
        ESP_LOGD("Monitor",
                 "METRICS: cpu=%u%% heap=%u dram=%u/%u psram=%u uptime=%us wifi=%s",
                 msg.cpuUsagePercent, msg.freeHeap, msg.freeDram, msg.totalDram,
                 msg.freePsram, msg.uptimeSeconds,
                 msg.wifiConnected ? "UP" : "DOWN");
    }

    static void onMqttConnected(const embed::MqttConnected& msg, void* /*ctx*/) {
        ESP_LOGI("Monitor", "MQTT CONNECTED to %s", msg.brokerUri.c_str());
    }

    static void onMqttDisconnected(const embed::MqttDisconnected& msg, void* /*ctx*/) {
        ESP_LOGI("Monitor", "MQTT DISCONNECTED, reason=%u", msg.reason);
    }

    static void onMqttMessage(const embed::MqttMessageReceived& msg, void* /*ctx*/) {
        ESP_LOGD("Monitor", "MQTT MSG: topic=%s len=%u", msg.topic.c_str(), msg.payload.size());
    }

    static void onWakeWordDetected(const embed::audio::WakeWordDetected& msg, void* /*ctx*/) {
        ESP_LOGI("Monitor", "WAKE WORD DETECTED cmd_id=%u", msg.wakeWordId);
    }

    static void onVoiceActivity(const embed::audio::VoiceActivityEvent& msg, void* /*ctx*/) {
        ESP_LOGD("Monitor", "VAD state=%u frame=%u", msg.vadState, msg.frameIndex);
    }

    embed::Slot<embed::WifiConnected>          wifi_connected_slot_{onWifiConnected, this};
    embed::Slot<embed::WifiDisconnected>       wifi_disconnected_slot_{onWifiDisconnected, this};
    embed::Slot<embed::MetricsCollected>       metrics_slot_{onMetricsCollected, this};
    embed::Slot<embed::MqttConnected>          mqtt_connected_slot_{onMqttConnected, this};
    embed::Slot<embed::MqttDisconnected>       mqtt_disconnected_slot_{onMqttDisconnected, this};
    embed::Slot<embed::MqttMessageReceived>    mqtt_message_slot_{onMqttMessage, this};
    embed::Slot<embed::audio::WakeWordDetected>    wake_word_slot_{onWakeWordDetected, this};
    embed::Slot<embed::audio::VoiceActivityEvent>  voice_activity_slot_{onVoiceActivity, this};
};
