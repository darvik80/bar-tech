#include "esp_crt_bundle.h"
#include "embed/embed.hpp"
#include "embed_core/wifi_service.hpp"
#include "embed_core/metrics_service.hpp"
#include "embed_core/mqtt_credentials.hpp"
#include "embed_core/mqtt_service.hpp"
#include "embed_core/nvs_store.hpp"
#include "embed_core/device_settings.hpp"
#include "embed_core/firmware_slot.hpp"
#include "cogitor_iot/cogitor_iot.hpp"
#include "audio_bus/audio_bus_service.hpp"
#include "voice_detection/voice_detection_service.hpp"
#include "display/display_service.hpp"
#include "session_manager_service.hpp"
#include "monitor_service.hpp"

#include "esp_tls.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

namespace {

constexpr char TAG[] = "Main";

constexpr char kAppModel[]   = "ESP32-S3";
constexpr char kAppName[]    = "BarFace";
constexpr char kAppVendor[]  = "bar-tech";
constexpr char kAppProtocol[] = "iot/v1";

} // namespace

// ── app_main ───────────────────────────────────────────────────────────

extern "C" void app_main() {
    ESP_LOGI(TAG, "BarFace → Cogitor IoT Platform (bar-tech)");

    embed::startFactoryResetGpioWatch();

    if (embed::NvsStore::initFlash() != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed");
        embed::failUnconfirmedFirmware("nvs init failed");
        return;
    }

    const bool rstBurst = embed::checkRstBurstFactoryReset();
    const bool gpioHold = embed::factoryResetGpioHeld();
    if (rstBurst || gpioHold) {
        ESP_LOGW(TAG, "factory reset — wiping fctry + config portal (rst=%d gpio=%d)",
                 rstBurst ? 1 : 0, gpioHold ? 1 : 0);
        embed::factoryResetSettings();
    }

    embed::checkCrashLoopRollback();

    esp_err_t ret = esp_tls_init_global_ca_store();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init global CA store: %s", esp_err_to_name(ret));
        embed::failUnconfirmedFirmware("tls ca store failed");
        return;
    }

    ESP_LOGI(TAG, "Loading CRT bundle...");
    ret = esp_crt_bundle_attach(NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set CA bundle: %s", esp_err_to_name(ret));
        embed::failUnconfirmedFirmware("crt bundle failed");
        return;
    }

    embed::EventLoop::instance().init();

    const bool portal = embed::needsConfigPortal();

    const auto topicStyle =
#ifdef CONFIG_EMBED_COGITOR_IOT_TOPIC_SHORT
        cogitor::iot::TopicStyle::Short;
#else
        cogitor::iot::TopicStyle::Full;
#endif

    static std::optional<cogitor::iot::CogitorCredentials> credentials;
    if (!portal) {
        credentials = cogitor::iot::loadOrSeedCredentials(
            CONFIG_EMBED_COGITOR_IOT_PRODUCT_ID,
            CONFIG_EMBED_COGITOR_IOT_DEVICE_ID,
            CONFIG_EMBED_COGITOR_IOT_HOST,
            CONFIG_EMBED_COGITOR_IOT_ACCESS_TOKEN,
            topicStyle,
#ifdef CONFIG_EMBED_COGITOR_IOT_USE_TLS
            true,
#else
            false,
#endif
            static_cast<uint16_t>(CONFIG_EMBED_COGITOR_IOT_PORT));
    }

    auto& registry = embed::ServiceRegistry::instance();
    auto* wifi = registry.createService<embed::WifiService>();
    if (portal) {
        wifi->enableSoftAp();
    }
    registry.createService<embed::MetricsService>();
    if (portal) registry.createService<cogitor::iot::ConfigPortalService>();

    const bool mqttOk = !portal && credentials && credentials->isValid();
    if (mqttOk) {
        ESP_LOGI(TAG, "Cogitor MQTT client_id=%s uri=%s style=%s",
                 credentials->clientId(), credentials->brokerUri(),
                 credentials->topicStyle() == cogitor::iot::TopicStyle::Short
                     ? "short"
                     : "full");
        registry.createService<embed::MqttService>(*credentials);
        registry.createService<cogitor::iot::IotService>(*credentials);
        //registry.createService<cogitor::iot::MetricsTelemetryBridge>();
        //registry.createService<cogitor::iot::OtaService>();
        //registry.createService<cogitor::iot::DeviceInfo>();
    } else if (!portal) {
        ESP_LOGW(TAG,
                 "Cogitor creds missing — config HTTP on STA; "
                 "or hold BOOT to open SoftAP portal");
    }

    // AudioBus — duplex I2S for ES8311 codec (shared by speaker + mic)
    registry.createService<embed::audio::AudioBusService>();
    // Voice detection — mic AFE + VAD + custom wake word (DS-CNN)
    registry.createService<embed::audio::VoiceDetectionService>();
    // Display — ST7789 SPI LCD with "Hello World" greeting
    registry.createService<embed::display::DisplayService>();
    // Monitor — logs WiFi, MQTT, metrics, voice events
    registry.createService<MonitorService>();
    // Session manager — orchestrates voice conversation flow
    registry.createService<embed::audio::SessionManagerService>();

    ESP_LOGI(TAG, "services=%zu free_heap=%lu portal=%d",
             registry.count(),
             static_cast<unsigned long>(esp_get_free_heap_size()),
             portal ? 1 : 0);

    registry.startAll();

    if (portal) {
        ESP_LOGW(TAG, "CONFIG PORTAL AP=%s → http://192.168.4.1/", wifi->apSsid());
    } else if (mqttOk) {
        ESP_LOGI(TAG, "running — WiFi+MQTT → Cogitor (%.*s.%.*s)",
                 static_cast<int>(credentials->productId().size()),
                 credentials->productId().data(),
                 static_cast<int>(credentials->deviceId().size()),
                 credentials->deviceId().data());
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10'000));
    }
}
