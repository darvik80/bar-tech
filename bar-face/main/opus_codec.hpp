#pragma once

#include <cstddef>
#include <cstdint>

namespace embed::audio {

/// Thin RAII wrapper around libopus encoder + decoder.
///
/// Configured for 16 kHz mono VOIP — matches the UDP audio protocol
/// (20 ms frames = 320 samples).
class OpusCodec {
public:
    OpusCodec() = default;
    ~OpusCodec() { destroy(); }

    OpusCodec(const OpusCodec&) = delete;
    OpusCodec& operator=(const OpusCodec&) = delete;

    /// Create encoder + decoder.
    /// @return true on success
    bool init(int sampleRate = 16000, int channels = 1);

    /// Release encoder and decoder handles.
    void destroy();

    bool isInitialized() const { return encoder_ != nullptr; }

    /// Encode one frame of PCM samples into OPUS.
    /// @param pcm        input PCM16 samples (frameSize samples)
    /// @param frameSize  number of samples per channel (e.g. 320 for 20 ms @ 16 kHz)
    /// @param outBuf     output buffer for encoded OPUS packet
    /// @param maxOutLen  capacity of outBuf
    /// @return encoded packet size in bytes, or negative on error
    int encode(const int16_t* pcm, int frameSize,
               uint8_t* outBuf, int maxOutLen);

    /// Decode one OPUS packet into PCM samples.
    /// @param data       encoded OPUS packet
    /// @param len        packet length in bytes
    /// @param pcm        output buffer for decoded PCM16 samples
    /// @param frameSize  expected number of samples per channel
    /// @return number of decoded samples, or negative on error
    int decode(const uint8_t* data, int len,
               int16_t* pcm, int frameSize);

    /// Maximum encoded packet size (generous upper bound for 20 ms @ 16 kHz).
    static constexpr int kMaxPacketSize = 256;

private:
    void* encoder_ = nullptr;  ///< OpusEncoder*
    void* decoder_ = nullptr;  ///< OpusDecoder*
};

} // namespace embed::audio
