#include "opus_codec.hpp"

#include "opus.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static constexpr char TAG[] = "OpusCodec";

namespace embed::audio {

bool OpusCodec::init(int sampleRate, int channels) {
    destroy();

    // Pre-allocate encoder/decoder in PSRAM to save internal RAM for task stacks
    int err = 0;
    int encSize = opus_encoder_get_size(channels);
    encoder_ = heap_caps_calloc(1, encSize, MALLOC_CAP_SPIRAM);
    if (!encoder_) {
        ESP_LOGE(TAG, "encoder PSRAM alloc failed (%d bytes)", encSize);
        return false;
    }
    err = opus_encoder_init(static_cast<OpusEncoder*>(encoder_),
                            sampleRate, channels, OPUS_APPLICATION_AUDIO);
    if (err != OPUS_OK) {
        ESP_LOGE(TAG, "opus_encoder_init failed: %d", err);
        heap_caps_free(encoder_);
        encoder_ = nullptr;
        return false;
    }
    auto* enc = static_cast<OpusEncoder*>(encoder_);
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(0));       // match xiaozhi — minimal CPU
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(OPUS_AUTO));  // adaptive bitrate
    opus_encoder_ctl(enc, OPUS_SET_VBR(1));
    opus_encoder_ctl(enc, OPUS_SET_DTX(1));

    int decSize = opus_decoder_get_size(channels);
    decoder_ = heap_caps_calloc(1, decSize, MALLOC_CAP_SPIRAM);
    if (!decoder_) {
        ESP_LOGE(TAG, "decoder PSRAM alloc failed (%d bytes)", decSize);
        heap_caps_free(encoder_);
        encoder_ = nullptr;
        return false;
    }
    err = opus_decoder_init(static_cast<OpusDecoder*>(decoder_),
                            sampleRate, channels);
    if (err != OPUS_OK) {
        ESP_LOGE(TAG, "opus_decoder_init failed: %d", err);
        heap_caps_free(decoder_);
        heap_caps_free(encoder_);
        encoder_ = nullptr;
        decoder_ = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "initialized — %d Hz, %d ch, AUDIO, auto-bitrate VBR+DTX "
             "(enc=%d bytes in PSRAM, dec=%d bytes in PSRAM)",
             sampleRate, channels, encSize, decSize);
    return true;
}

void OpusCodec::destroy() {
    if (encoder_) {
        heap_caps_free(encoder_);
        encoder_ = nullptr;
    }
    if (decoder_) {
        heap_caps_free(decoder_);
        decoder_ = nullptr;
    }
}

int OpusCodec::encode(const int16_t* pcm, int frameSize,
                      uint8_t* outBuf, int maxOutLen) {
    if (!encoder_ || !pcm || !outBuf || frameSize <= 0 || maxOutLen <= 0) {
        return -1;
    }
    return opus_encode(static_cast<OpusEncoder*>(encoder_),
                       pcm, frameSize, outBuf, maxOutLen);
}

int OpusCodec::decode(const uint8_t* data, int len,
                      int16_t* pcm, int frameSize) {
    if (!decoder_ || !data || !pcm || len <= 0 || frameSize <= 0) {
        return -1;
    }
    return opus_decode(static_cast<OpusDecoder*>(decoder_),
                       data, len, pcm, frameSize, 0);
}

} // namespace embed::audio
