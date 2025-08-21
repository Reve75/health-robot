#ifndef DUAL_MIC_CODEC_H
#define DUAL_MIC_CODEC_H

#include "audio_codecs/no_audio_codec.h"
#include <driver/gpio.h>

// Dual microphone codec that supports stereo input from two I2S microphones
// Both microphones share the same WS, SCK, and SD pins
// L/R pin on each mic determines channel (GND=left, 3.3V=right)
class DualMicStereoCodec : public NoAudioCodec {
private:
    bool dual_mic_enabled_ = false;
    
public:
    DualMicStereoCodec(int input_sample_rate, int output_sample_rate,
                       gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout,
                       gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din,
                       bool enable_dual_mic = false);
    
    virtual void EnableOutput(bool enable) override;
    virtual void Start() override;
    
    bool IsDualMicEnabled() const {
        return dual_mic_enabled_;
    }
};

#endif // DUAL_MIC_CODEC_H