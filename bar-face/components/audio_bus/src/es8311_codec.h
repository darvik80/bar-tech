#pragma once

#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"

/// Minimal register-level ES8311 codec driver.
///
/// Init sequence replicates espressif/esp_codec_dev (device/es8311/es8311.c)
/// for: codec as I2S slave, MCLK from pad = 256 * fs, 16-bit I2S (Philips)
/// format, ADC (mic) + DAC (speaker) enabled, DAC unmuted.
///
/// The I2C master bus is created and owned by the caller (AudioBusService).
/// Sample rate must have an MCLK = 256 * fs row in the coefficient table
/// (8k/16k/24k/48k are supported).

esp_err_t es8311_init(i2c_master_bus_handle_t bus, uint8_t devAddr,
                      uint32_t sampleRate);

/// Mic PGA gain in dB (0..42, rounded to 6 dB steps like the official driver).
esp_err_t es8311_set_mic_gain(int db);

/// DAC volume register 0x00..0xFF (0 = -95.5 dB, 0xFF = +32 dB, linear).
esp_err_t es8311_set_dac_volume(uint8_t reg);

/// Unmute (false) or mute (true) the DAC output.
esp_err_t es8311_set_dac_mute(bool mute);
