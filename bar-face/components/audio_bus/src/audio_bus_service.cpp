#include "audio_bus/audio_bus_service.hpp"

#include "es8311_codec.h"

#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <cmath>

static constexpr char TAG[] = "AudioBus";

// ── I2S pin mapping (ES8311 audio codec — shared duplex bus) ───────────

static constexpr gpio_num_t kI2sMclk = static_cast<gpio_num_t>(CONFIG_EMBED_AUDIO_I2S_MCLK);
static constexpr gpio_num_t kI2sBck  = static_cast<gpio_num_t>(CONFIG_EMBED_AUDIO_I2S_BCK);
static constexpr gpio_num_t kI2sWs   = static_cast<gpio_num_t>(CONFIG_EMBED_AUDIO_I2S_WS);
static constexpr gpio_num_t kI2sDout = static_cast<gpio_num_t>(CONFIG_EMBED_AUDIO_I2S_DOUT);
static constexpr gpio_num_t kI2sDin  = static_cast<gpio_num_t>(CONFIG_EMBED_AUDIO_I2S_DIN);
static constexpr int        kAmpEn   = CONFIG_EMBED_AUDIO_AMP_EN_GPIO;

// ── Codec control (I2C) ───────────────────────────────────────────────

static constexpr gpio_num_t kI2cSda  = static_cast<gpio_num_t>(CONFIG_EMBED_AUDIO_I2C_SDA);
static constexpr gpio_num_t kI2cScl  = static_cast<gpio_num_t>(CONFIG_EMBED_AUDIO_I2C_SCL);
static constexpr uint8_t    kCodecAddr = CONFIG_EMBED_AUDIO_CODEC_ADDR;
static constexpr int        kMicGainDb = CONFIG_EMBED_AUDIO_MIC_GAIN_DB;
static constexpr uint8_t    kDacVolume = CONFIG_EMBED_AUDIO_DAC_VOLUME;

namespace embed::audio {

// ── Duplex I2S init ────────────────────────────────────────────────────

bool AudioBusService::initDuplexI2s() {
    // Single I2S controller with both TX and RX channels (full-duplex)
    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chanCfg.dma_desc_num  = 6;
    chanCfg.dma_frame_num = 240;
    chanCfg.auto_clear_after_cb = true;  // TX sends zeros on underrun instead of looping

    i2s_chan_handle_t tx = nullptr;
    i2s_chan_handle_t rx = nullptr;
    esp_err_t ret = i2s_new_channel(&chanCfg, &tx, &rx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel(duplex) failed: %s", esp_err_to_name(ret));
        return false;
    }

    // TX config (speaker output)
    i2s_std_config_t txCfg{};
    txCfg.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
    txCfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    txCfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    txCfg.gpio_cfg.bclk = kI2sBck;
    txCfg.gpio_cfg.ws   = kI2sWs;
    txCfg.gpio_cfg.dout = kI2sDout;
    txCfg.gpio_cfg.din  = I2S_GPIO_UNUSED;
    txCfg.gpio_cfg.mclk = kI2sMclk;

    ret = i2s_channel_init_std_mode(tx, &txCfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode(TX) failed: %s", esp_err_to_name(ret));
        i2s_del_channel(tx);
        i2s_del_channel(rx);
        return false;
    }

    // RX config (microphone input)
    i2s_std_config_t rxCfg{};
    rxCfg.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
    rxCfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    rxCfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    rxCfg.gpio_cfg.bclk = kI2sBck;
    rxCfg.gpio_cfg.ws   = kI2sWs;
    rxCfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    rxCfg.gpio_cfg.din  = kI2sDin;
    rxCfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;

    ret = i2s_channel_init_std_mode(rx, &rxCfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode(RX) failed: %s", esp_err_to_name(ret));
        i2s_del_channel(tx);
        i2s_del_channel(rx);
        return false;
    }

    // Enable both channels
    ret = i2s_channel_enable(tx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable(TX) failed: %s", esp_err_to_name(ret));
        i2s_del_channel(tx);
        i2s_del_channel(rx);
        return false;
    }

    ret = i2s_channel_enable(rx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable(RX) failed: %s", esp_err_to_name(ret));
        i2s_channel_disable(tx);
        i2s_del_channel(tx);
        i2s_del_channel(rx);
        return false;
    }

    txHandle_ = static_cast<void*>(tx);
    rxHandle_ = static_cast<void*>(rx);

    ESP_LOGI(TAG, "duplex I2S ready: MCLK=%d BCK=%d WS=%d DOUT=%d DIN=%d %dHz/16-bit",
             kI2sMclk, kI2sBck, kI2sWs, kI2sDout, kI2sDin, kSampleRate);
    return true;
}

// ── Codec init (ES8311 over I2C) ─────────────────────────────────────

bool AudioBusService::initCodec() {
    // I2C master bus for codec control
    i2c_master_bus_config_t busCfg = {};
    busCfg.i2c_port = I2C_NUM_0;
    busCfg.sda_io_num = kI2cSda;
    busCfg.scl_io_num = kI2cScl;
    busCfg.clk_source = I2C_CLK_SRC_DEFAULT;
    busCfg.glitch_ignore_cnt = 7;
    busCfg.flags.enable_internal_pullup = true;

    auto bus = static_cast<i2c_master_bus_handle_t>(i2cBus_);
    esp_err_t ret = i2c_new_master_bus(&busCfg, &bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(ret));
        return false;
    }
    i2cBus_ = static_cast<void*>(bus);

    ret = es8311_init(bus, kCodecAddr, kSampleRate);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 init failed: %s", esp_err_to_name(ret));
        return false;
    }

    // Mic PGA gain and DAC output volume
    ret = es8311_set_mic_gain(kMicGainDb);
    ret |= es8311_set_dac_volume(kDacVolume);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "codec gain/volume setup failed");
        return false;
    }

    ESP_LOGI(TAG, "ES8311 configured: mic=+%d dB dac_vol=0x%02X",
             kMicGainDb, kDacVolume);
    return true;
}

// ── Service lifecycle ──────────────────────────────────────────────────

void AudioBusService::start() {
    ESP_LOGI(TAG, "audio bus starting...");

    if (!initDuplexI2s()) {
        ESP_LOGE(TAG, "duplex I2S init failed");
        return;
    }

    if (!initCodec()) {
        ESP_LOGE(TAG, "codec init failed");
        return;
    }

    // Enable amplifier
    if (kAmpEn >= 0) {
        gpio_config_t ioCfg{};
        ioCfg.pin_bit_mask = (1ULL << kAmpEn);
        ioCfg.mode         = GPIO_MODE_OUTPUT;
        gpio_config(&ioCfg);
        gpio_set_level(static_cast<gpio_num_t>(kAmpEn), 1);
        ESP_LOGI(TAG, "amplifier enabled (GPIO %d)", kAmpEn);
    }

    ESP_LOGI(TAG, "audio bus ready");
}

void AudioBusService::stop() {
    // Mute DAC and disable amplifier
    es8311_set_dac_mute(true);
    if (kAmpEn >= 0) {
        gpio_set_level(static_cast<gpio_num_t>(kAmpEn), 0);
    }

    if (txHandle_) {
        i2s_channel_disable(static_cast<i2s_chan_handle_t>(txHandle_));
        i2s_del_channel(static_cast<i2s_chan_handle_t>(txHandle_));
        txHandle_ = nullptr;
    }
    if (rxHandle_) {
        i2s_channel_disable(static_cast<i2s_chan_handle_t>(rxHandle_));
        i2s_del_channel(static_cast<i2s_chan_handle_t>(rxHandle_));
        rxHandle_ = nullptr;
    }

    ESP_LOGI(TAG, "audio bus stopped");
}

// ── play() ─────────────────────────────────────────────────────────────

bool AudioBusService::play(const int16_t* samples, size_t count) {
    if (!txHandle_ || !samples || count == 0) return false;

    auto* handle = static_cast<i2s_chan_handle_t>(txHandle_);
    const size_t totalBytes = count * sizeof(int16_t);
    size_t bytesWritten = 0;
    esp_err_t ret = i2s_channel_write(handle, samples, totalBytes,
                                       &bytesWritten, pdMS_TO_TICKS(5000));
    return ret == ESP_OK && bytesWritten == totalBytes;
}

// ── record() ───────────────────────────────────────────────────────────

size_t AudioBusService::record(int16_t* buffer, size_t count) {
    if (!rxHandle_ || !buffer || count == 0) return 0;

    auto* handle = static_cast<i2s_chan_handle_t>(rxHandle_);
    const size_t totalBytes = count * sizeof(int16_t);
    size_t bytesRead = 0;
    esp_err_t ret = i2s_channel_read(handle, buffer, totalBytes,
                                      &bytesRead, pdMS_TO_TICKS(1000));
    if (ret != ESP_OK) return 0;
    return bytesRead / sizeof(int16_t);
}

// ── playBeep() ─────────────────────────────────────────────────────────

void AudioBusService::playBeep(int freqHz, int durationMs) {
    const int numSamples = kSampleRate * durationMs / 1000;
    constexpr int chunkSize = 240;

    int16_t chunk[chunkSize];
    int remaining = numSamples;
    int sampleIdx = 0;

    while (remaining > 0) {
        int n = (remaining < chunkSize) ? remaining : chunkSize;
        for (int i = 0; i < n; ++i, ++sampleIdx) {
            float t = static_cast<float>(sampleIdx) / kSampleRate;
            chunk[i] = static_cast<int16_t>(
                32000.0f * sinf(2.0f * static_cast<float>(M_PI) * freqHz * t));
        }
        play(chunk, static_cast<size_t>(n));
        remaining -= n;
    }
}

// ── speak() ────────────────────────────────────────────────────────────

bool AudioBusService::speak(const char* pinyin) {
    ESP_LOGW(TAG, "TTS not available — cannot speak: %s", pinyin);
    return false;
}

} // namespace embed::audio
