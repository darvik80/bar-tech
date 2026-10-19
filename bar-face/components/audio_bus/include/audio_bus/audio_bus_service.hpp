#pragma once

#include <cstdint>
#include <cstddef>

#include "embed/service.hpp"

namespace embed::audio {

/// Единый владелец duplex I2S шины для ES8311 аудиокодека.
///
/// Предоставляет API для воспроизведения (play) и записи (record) через
/// общий I2S порт с разделяемыми часами (MCLK, BCLK, WS).
/// VoiceDetectionService и другие потребители используют этот сервис вместо
/// независимых I2S-контроллеров, что устраняет конфликт GPIO и обеспечивает
/// единую тактовую область для ES8311.
class AudioBusService : public embed::Service {
public:
    AudioBusService() = default;
    const char* serviceName() const override { return "AudioBusService"; }

    void start() override;
    void stop() override;

    /// Воспроизвести PCM-сэмплы (16-bit signed, mono).
    /// Блокирующий вызов — ждёт завершения записи в DMA-буфер.
    /// @param samples  указатель на массив сэмплов
    /// @param count    количество сэмплов (не байт)
    /// @return true если все сэмплы успешно записаны
    bool play(const int16_t* samples, size_t count);

    /// Прочитать PCM-сэмплы с микрофона (16-bit signed, mono).
    /// Блокирующий вызов — ждёт поступления данных из DMA-буфера.
    /// @param buffer   указатель на буфер для приёма
    /// @param count    количество сэмплов для чтения
    /// @return количество прочитанных сэмплов (может быть меньше count при ошибке)
    size_t record(int16_t* buffer, size_t count);

    /// Play a sine-wave beep tone (blocking).
    /// @param freqHz     frequency in Hz (e.g. 440)
    /// @param durationMs duration in milliseconds
    void playBeep(int freqHz, int durationMs);

    /// Speak pinyin text (stub — not implemented).
    /// @return false always
    bool speak(const char* pinyin);

    /// Проверить, инициализирована ли шина.
    bool isReady() const { return txHandle_ != nullptr && rxHandle_ != nullptr; }

    /// Получить частоту дискретизации (Гц).
    int sampleRate() const { return kSampleRate; }

private:
    bool initDuplexI2s();
    bool initCodec();

    void* txHandle_ = nullptr;  ///< i2s_chan_handle_t для TX (динамик)
    void* rxHandle_ = nullptr;  ///< i2s_chan_handle_t для RX (микрофон)
    void* i2cBus_   = nullptr;  ///< i2c_master_bus_handle_t (шина кодека)
    void* i2cDev_   = nullptr;  ///< i2c_master_dev_handle_t (ES8311)

    static constexpr int kSampleRate = 16000;
};

} // namespace embed::audio
