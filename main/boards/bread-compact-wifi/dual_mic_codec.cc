#include "dual_mic_codec.h"
#include "audio_codec.h"
#include <driver/i2s_std.h>
#include <esp_log.h>

#define TAG "DualMicCodec"

// Custom stereo codec implementation
DualMicStereoCodec::DualMicStereoCodec(int input_sample_rate, int output_sample_rate,
                                       gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout,
                                       gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din,
                                       bool enable_dual_mic) 
    : dual_mic_enabled_(enable_dual_mic) {
    
    duplex_ = false;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    
    // Set channel configuration based on mode
    if (dual_mic_enabled_) {
        input_channels_ = 2;
        input_reference_ = true;  // Second mic can be used as reference for AEC
        ESP_LOGI(TAG, "Initializing DUAL microphone mode (stereo)");
    } else {
        input_channels_ = 1;
        input_reference_ = false;
        ESP_LOGI(TAG, "Initializing SINGLE microphone mode (mono)");
    }
    
    // Create speaker channel (always mono output)
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, nullptr));
    
    // Configure speaker (output) - always mono
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
#ifdef I2S_HW_VERSION_2
            .ext_clk_freq_hz = 0,
#endif
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_BOTH,  // Output mono to both L&R
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
#ifdef I2S_HW_VERSION_2
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false
#endif
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = spk_bclk,
            .ws = spk_ws,
            .dout = spk_dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_LOGI(TAG, "Speaker channel initialized (mono output)");
    
    // Create microphone channel
    chan_cfg.id = I2S_NUM_1;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, nullptr, &rx_handle_));
    
    // Configure microphone (input) - mono or stereo based on mode
    std_cfg.clk_cfg.sample_rate_hz = (uint32_t)input_sample_rate_;
    
    if (dual_mic_enabled_) {
        // STEREO configuration for dual microphones
        std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
        std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;  // Read both L&R channels
        ESP_LOGI(TAG, "Microphone channel configured for STEREO (both channels)");
    } else {
        // MONO configuration for single microphone  
        std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_MONO;
        std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;  // Read only left channel
        ESP_LOGI(TAG, "Microphone channel configured for MONO (left channel only)");
    }
    
    std_cfg.gpio_cfg.bclk = mic_sck;
    std_cfg.gpio_cfg.ws = mic_ws;
    std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.din = mic_din;
    
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    ESP_LOGI(TAG, "Microphone channel initialized - WS:%d, SCK:%d, SD:%d", 
             mic_ws, mic_sck, mic_din);
    
    if (dual_mic_enabled_) {
        ESP_LOGI(TAG, "=== DUAL MIC SETUP COMPLETE ===");
        ESP_LOGI(TAG, "Mic 1: Connect L/R to GND (left channel)");
        ESP_LOGI(TAG, "Mic 2: Connect L/R to 3.3V (right channel)");
        ESP_LOGI(TAG, "Both mics share: WS=%d, SCK=%d, SD=%d", mic_ws, mic_sck, mic_din);
    }
}

void DualMicStereoCodec::Start() {
    // Enable both I2S channels
    if (tx_handle_) {
        esp_err_t err = i2s_channel_enable(tx_handle_);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Speaker I2S channel enabled");
        } else if (err == ESP_ERR_INVALID_STATE) {
            ESP_LOGD(TAG, "Speaker I2S channel already enabled");
        } else {
            ESP_LOGE(TAG, "Failed to enable speaker I2S channel: %s", esp_err_to_name(err));
        }
    }
    
    if (rx_handle_) {
        esp_err_t err = i2s_channel_enable(rx_handle_);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Microphone I2S channel enabled (channels=%d)", input_channels_);
        } else if (err == ESP_ERR_INVALID_STATE) {
            ESP_LOGD(TAG, "Microphone I2S channel already enabled");
        } else {
            ESP_LOGE(TAG, "Failed to enable microphone I2S channel: %s", esp_err_to_name(err));
        }
    }
    
    input_enabled_ = true;
    output_enabled_ = false;  // Speaker starts disabled
}

void DualMicStereoCodec::EnableOutput(bool enable) {
    // Only change state if different from current state
    if (output_enabled_ == enable) {
        return;  // Already in the requested state
    }
    
    // Call parent class EnableOutput
    if (tx_handle_) {
        if (enable) {
            esp_err_t err = i2s_channel_enable(tx_handle_);
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                ESP_LOGE(TAG, "Failed to enable I2S channel: %s", esp_err_to_name(err));
            }
        } else {
            esp_err_t err = i2s_channel_disable(tx_handle_);
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                ESP_LOGE(TAG, "Failed to disable I2S channel: %s", esp_err_to_name(err));
            }
        }
        output_enabled_ = enable;
    }
#ifdef SPEAKER_SD_PIN
    // Control MAX98357 SD pin
    gpio_set_level(SPEAKER_SD_PIN, enable ? 1 : 0);
    ESP_LOGI(TAG, "MAX98357 SD pin set to %s (GPIO %d = %d)", 
             enable ? "ENABLED" : "SHUTDOWN", SPEAKER_SD_PIN, enable ? 1 : 0);
#endif
}