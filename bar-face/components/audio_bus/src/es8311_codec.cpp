#include "es8311_codec.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static constexpr char TAG[] = "ES8311";

// ── Register map (from espressif/esp_codec_dev device/es8311/es8311_reg.h) ─

enum : uint8_t {
    ES8311_RESET_REG00       = 0x00, // reset digital, csm, clock manager
    ES8311_CLK_MANAGER_REG01 = 0x01, // mclk src select, codec clock enable
    ES8311_CLK_MANAGER_REG02 = 0x02, // clk divider and multiplier
    ES8311_CLK_MANAGER_REG03 = 0x03, // adc fsmode and osr
    ES8311_CLK_MANAGER_REG04 = 0x04, // dac osr
    ES8311_CLK_MANAGER_REG05 = 0x05, // clk divider for adc and dac
    ES8311_CLK_MANAGER_REG06 = 0x06, // bclk inverter and divider
    ES8311_CLK_MANAGER_REG07 = 0x07, // lrck divider high
    ES8311_CLK_MANAGER_REG08 = 0x08, // lrck divider low
    ES8311_SDPIN_REG09       = 0x09, // dac serial digital port (codec DIN)
    ES8311_SDPOUT_REG0A      = 0x0A, // adc serial digital port (codec DOUT)
    ES8311_SYSTEM_REG0B      = 0x0B,
    ES8311_SYSTEM_REG0C      = 0x0C,
    ES8311_SYSTEM_REG0D      = 0x0D, // power up/down
    ES8311_SYSTEM_REG0E      = 0x0E, // power up/down
    ES8311_SYSTEM_REG10      = 0x10,
    ES8311_SYSTEM_REG11      = 0x11,
    ES8311_SYSTEM_REG12      = 0x12, // enable DAC
    ES8311_SYSTEM_REG13      = 0x13,
    ES8311_SYSTEM_REG14      = 0x14, // DMIC select, analog pga gain
    ES8311_ADC_REG15         = 0x15, // adc ramp rate, dmic sense
    ES8311_ADC_REG16         = 0x16, // mic gain scale (0..7 => 0..42 dB)
    ES8311_ADC_REG17         = 0x17, // adc volume
    ES8311_ADC_REG1B         = 0x1B, // adc hpf s1
    ES8311_ADC_REG1C         = 0x1C, // hpf s2
    ES8311_DAC_REG31         = 0x31, // dac mute
    ES8311_DAC_REG32         = 0x32, // dac volume
    ES8311_DAC_REG37         = 0x37, // dac ramprate
    ES8311_GPIO_REG44        = 0x44, // gpio, dac2adc for test
    ES8311_GP_REG45          = 0x45,
    ES8311_CHVER_REGFF       = 0xFF, // chip version
};

// ── I2C helpers ────────────────────────────────────────────────────────────

static i2c_master_dev_handle_t s_dev = nullptr;

// ES8311 errata (see esp_codec_dev): occasional single I2C NACKs happen.
// Retry a few times before declaring the transaction failed.
static constexpr int kI2cRetries = 3;

static esp_err_t esWrite(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    esp_err_t ret = ESP_FAIL;
    for (int attempt = 0; attempt < kI2cRetries; ++attempt) {
        ret = i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
        if (ret == ESP_OK) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    ESP_LOGE(TAG, "write reg 0x%02X = 0x%02X failed: %s",
             reg, val, esp_err_to_name(ret));
    return ret;
}

static esp_err_t esRead(uint8_t reg, uint8_t* val) {
    esp_err_t ret = ESP_FAIL;
    for (int attempt = 0; attempt < kI2cRetries; ++attempt) {
        ret = i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 100);
        if (ret == ESP_OK) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    ESP_LOGE(TAG, "read reg 0x%02X failed: %s", reg, esp_err_to_name(ret));
    return ret;
}

static esp_err_t esUpdate(uint8_t reg, uint8_t clearMask, uint8_t setMask) {
    uint8_t v = 0;
    esp_err_t ret = esRead(reg, &v);
    if (ret != ESP_OK) return ret;
    v = (v & ~clearMask) | setMask;
    return esWrite(reg, v);
}

// ── Clock coefficient table (subset for MCLK = 256 * fs) ──────────────────

struct CoeffDiv {
    uint32_t mclk;
    uint32_t rate;
    uint8_t  preDiv;    // pre divider, 1..8
    uint8_t  preMulti;  // pre multiplier, 1/2/4/8
    uint8_t  adcDiv;    // adcclk divider
    uint8_t  dacDiv;    // dacclk divider
    uint8_t  fsMode;    // 0 = single speed, 1 = double speed
    uint8_t  lrckH;     // lrck divider high
    uint8_t  lrckL;     // lrck divider low
    uint8_t  bclkDiv;   // sclk divider
    uint8_t  adcOsr;    // adc oversample rate
    uint8_t  dacOsr;    // dac oversample rate
};

static constexpr CoeffDiv kCoeffDiv[] = {
    // mclk      rate   preD  mult  adcD dacD fsM  lrch  lrcl  bckD osrA osrD
    {2048000,   8000,  0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20},
    {4096000,  16000,  0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x20},
    {6144000,  24000,  0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {12288000, 48000,  0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
};

// ── Mic gain (enum values 0..7 => 0..42 dB, from official driver) ──────────

static int micGainDbToReg(int db) {
    if (db < 6)  return 0;
    if (db < 12) return 1;
    if (db < 18) return 2;
    if (db < 24) return 3;
    if (db < 30) return 4;
    if (db < 36) return 5;
    if (db < 42) return 6;
    return 7;
}

// ── Init sequence ──────────────────────────────────────────────────────────

esp_err_t es8311_init(i2c_master_bus_handle_t bus, uint8_t devAddr,
                      uint32_t sampleRate) {
    // Attach to the caller-provided I2C bus
    i2c_device_config_t devCfg = {};
    devCfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    devCfg.device_address  = devAddr;
    devCfg.scl_speed_hz    = 100 * 1000;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &devCfg, &s_dev),
                        TAG, "i2c device add failed");

    // Probe: read chip version — fails immediately if wiring/address is wrong
    uint8_t ver = 0;
    esp_err_t ret = esRead(ES8311_CHVER_REGFF, &ver);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "no ACK on 0x%02X — check SDA/SCL wiring and I2C address",
                 devAddr);
        return ret;
    }
    ESP_LOGI(TAG, "ES8311 detected at 0x%02X, chip version 0x%02X", devAddr, ver);

    // Software reset: hold digital blocks in reset (per init guide)
    ret = esWrite(ES8311_RESET_REG00, 0x1F);
    vTaskDelay(pdMS_TO_TICKS(5));

    // ── es8311_open (official driver) ──
    uint8_t reg0D = 0;
    esRead(ES8311_SYSTEM_REG0D, &reg0D);
    if (reg0D != 0xFA) {
        ret |= esWrite(ES8311_SYSTEM_REG0D, 0xFA);
    }
    // I2C noise immunity; the first write may occasionally fail — write twice
    ret |= esWrite(ES8311_GPIO_REG44, 0x08);
    ret |= esWrite(ES8311_GPIO_REG44, 0x08);

    ret |= esWrite(ES8311_CLK_MANAGER_REG01, 0x30);
    ret |= esWrite(ES8311_CLK_MANAGER_REG02, 0x00);
    ret |= esWrite(ES8311_CLK_MANAGER_REG03, 0x10);
    ret |= esWrite(ES8311_ADC_REG16, 0x24);
    ret |= esWrite(ES8311_CLK_MANAGER_REG04, 0x10);
    ret |= esWrite(ES8311_CLK_MANAGER_REG05, 0x00);
    ret |= esWrite(ES8311_SYSTEM_REG0B, 0x00);
    ret |= esWrite(ES8311_SYSTEM_REG0C, 0x00);
    ret |= esWrite(ES8311_SYSTEM_REG10, 0x1F);
    ret |= esWrite(ES8311_SYSTEM_REG11, 0x7F);

    // Reset release; codec works in slave mode (bit6 = 0)
    ret |= esWrite(ES8311_RESET_REG00, 0x80);

    // Clock source: MCLK from pad, not inverted
    ret |= esWrite(ES8311_CLK_MANAGER_REG01, 0x3F);
    // SCLK not inverted
    ret |= esUpdate(ES8311_CLK_MANAGER_REG06, 0x20, 0x00);

    ret |= esWrite(ES8311_SYSTEM_REG13, 0x10);
    ret |= esWrite(ES8311_ADC_REG1B, 0x0A);
    ret |= esWrite(ES8311_ADC_REG1C, 0x6A);
    // Internal reference signal (ADCL + DACR)
    ret |= esWrite(ES8311_GPIO_REG44, 0x58);

    // ── es8311_set_fs: 16-bit, I2S (Philips) format ──
    // bits=16: both serial ports get 0x0C; fmt=I2S normal: mask 0xFC
    ret |= esWrite(ES8311_SDPIN_REG09, 0x0C);
    ret |= esWrite(ES8311_SDPOUT_REG0A, 0x0C);

    // ── es8311_config_sample ──
    const uint32_t mclk = sampleRate * 256;
    const CoeffDiv* co = nullptr;
    for (const auto& c : kCoeffDiv) {
        if (c.mclk == mclk && c.rate == sampleRate) { co = &c; break; }
    }
    if (!co) {
        ESP_LOGE(TAG, "unsupported rate %lu Hz (MCLK %lu Hz)",
                 static_cast<unsigned long>(sampleRate),
                 static_cast<unsigned long>(mclk));
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t multiCode = 0; // 1:x1 1:x2 2:x4 3:x8
    if (co->preMulti == 2) multiCode = 1;
    else if (co->preMulti == 4) multiCode = 2;
    else if (co->preMulti == 8) multiCode = 3;

    uint8_t v = 0;
    ret |= esRead(ES8311_CLK_MANAGER_REG02, &v);
    v = (v & 0x07) | static_cast<uint8_t>((co->preDiv - 1) << 5) |
        static_cast<uint8_t>(multiCode << 3);
    ret |= esWrite(ES8311_CLK_MANAGER_REG02, v);

    v = static_cast<uint8_t>((co->adcDiv - 1) << 4) | static_cast<uint8_t>(co->dacDiv - 1);
    ret |= esWrite(ES8311_CLK_MANAGER_REG05, v);

    ret |= esRead(ES8311_CLK_MANAGER_REG03, &v);
    v = (v & 0x80) | static_cast<uint8_t>(co->fsMode << 6) | co->adcOsr;
    ret |= esWrite(ES8311_CLK_MANAGER_REG03, v);

    ret |= esRead(ES8311_CLK_MANAGER_REG04, &v);
    v = (v & 0x80) | co->dacOsr;
    ret |= esWrite(ES8311_CLK_MANAGER_REG04, v);

    ret |= esRead(ES8311_CLK_MANAGER_REG07, &v);
    v = (v & 0xC0) | co->lrckH;
    ret |= esWrite(ES8311_CLK_MANAGER_REG07, v);

    ret |= esWrite(ES8311_CLK_MANAGER_REG08, co->lrckL);

    ret |= esRead(ES8311_CLK_MANAGER_REG06, &v);
    v = (v & 0xE0) |
        static_cast<uint8_t>(co->bclkDiv < 19 ? co->bclkDiv - 1 : co->bclkDiv);
    ret |= esWrite(ES8311_CLK_MANAGER_REG06, v);

    // ── es8311_start ──
    ret |= esWrite(ES8311_RESET_REG00, 0x80);   // slave mode
    ret |= esWrite(ES8311_CLK_MANAGER_REG01, 0x3F); // mclk from pad
    // Clear power-down (bit6) on both serial data ports
    ret |= esUpdate(ES8311_SDPIN_REG09, 0x40, 0x00);
    ret |= esUpdate(ES8311_SDPOUT_REG0A, 0x40, 0x00);

    ret |= esWrite(ES8311_ADC_REG17, 0xBF);     // ADC digital volume max
    ret |= esWrite(ES8311_SYSTEM_REG0E, 0x02);
    ret |= esWrite(ES8311_SYSTEM_REG12, 0x00);  // enable DAC (BOTH mode)
    ret |= esWrite(ES8311_SYSTEM_REG14, 0x1A);
    ret |= esUpdate(ES8311_SYSTEM_REG14, 0x40, 0x00); // analog mic (DMIC off)

    ret |= esWrite(ES8311_SYSTEM_REG0D, 0x01);
    ret |= esWrite(ES8311_ADC_REG15, 0x40);
    ret |= esWrite(ES8311_DAC_REG37, 0x08);
    ret |= esWrite(ES8311_GP_REG45, 0x00);

    // Unmute DAC
    ret |= esUpdate(ES8311_DAC_REG31, 0x60, 0x00);

    ESP_RETURN_ON_FALSE(ret == ESP_OK, ESP_FAIL, TAG, "register writes failed");

    ESP_LOGI(TAG, "ES8311 ready: %lu Hz / 16-bit / I2S slave / MCLK=256fs",
             static_cast<unsigned long>(sampleRate));
    return ESP_OK;
}

esp_err_t es8311_set_mic_gain(int db) {
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    return esWrite(ES8311_ADC_REG16, static_cast<uint8_t>(micGainDbToReg(db)));
}

esp_err_t es8311_set_dac_volume(uint8_t reg) {
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    return esWrite(ES8311_DAC_REG32, reg);
}

esp_err_t es8311_set_dac_mute(bool mute) {
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    return esUpdate(ES8311_DAC_REG31, 0x60, mute ? 0x60 : 0x00);
}
