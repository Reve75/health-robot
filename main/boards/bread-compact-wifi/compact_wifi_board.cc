#include "wifi_board.h"
#include "audio_codecs/no_audio_codec.h"
#include "dual_mic_codec.h"
#include "display/oled_display.h"
#include "display/display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "iot/thing_manager.h"
#include "led/single_led.h"
#include "assets/lang_config.h"
#include "dual_oled_display.h"
#include <lvgl.h>

#include <wifi_station.h>
#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <memory>

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#define TAG "CompactWifiBoard"

LV_FONT_DECLARE(font_puhui_14_1);
LV_FONT_DECLARE(font_awesome_14_1);

// Forward declaration for CompactWifiBoard
class CompactWifiBoard;

class CompactWifiBoard : public WifiBoard {
public:
    
private:
    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    
    // Dual display support using new DualOledDisplay class
    bool use_dual_display_ = true;  // ENABLED with new implementation
    
    // Dual microphone support
    bool use_dual_microphone_ = true;  // ENABLED for dual mic mode
    
    Button boot_button_;
    Button touch_button_;
    Button volume_up_button_;
    Button volume_down_button_;

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeSsd1306Display() {
        // SSD1306 config
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
            .scl_speed_hz = 400 * 1000,
        };

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(display_i2c_bus_, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install SSD1306 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = -1;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

#ifdef SH1106
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
#endif
        ESP_LOGI(TAG, "SSD1306 driver installed");

        // Reset the display
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));

        // Set the display to on
        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));
        
        // Test pattern to verify display is working
        ESP_LOGI(TAG, "Drawing test pattern to verify display");
        // Calculate buffer size based on display dimensions
        size_t buffer_size = (128 * DISPLAY_HEIGHT) / 8;  // 1 bit per pixel
        uint8_t* test_buffer = new uint8_t[buffer_size];
        
        // Fill entire buffer with pattern to light up all pixels
        memset(test_buffer, 0xFF, buffer_size);  // All pixels on
        ESP_LOGI(TAG, "Drawing all pixels on (buffer size: %d bytes)", buffer_size);
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel_, 0, 0, 128, DISPLAY_HEIGHT, test_buffer));
        vTaskDelay(pdMS_TO_TICKS(2000));  // Show for 2 seconds
        
        // Draw stripes pattern
        for (size_t i = 0; i < buffer_size; i++) {
            test_buffer[i] = 0xAA;  // 10101010 pattern
        }
        ESP_LOGI(TAG, "Drawing stripe pattern");
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel_, 0, 0, 128, DISPLAY_HEIGHT, test_buffer));
        vTaskDelay(pdMS_TO_TICKS(2000));  // Show for 2 seconds
        
        // Clear display
        memset(test_buffer, 0, buffer_size);
        ESP_LOGI(TAG, "Clearing display");
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel_, 0, 0, 128, DISPLAY_HEIGHT, test_buffer));
        
        delete[] test_buffer;

        // This part is now handled in the constructor above
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState();
        });
        // Simple medication confirmation - play prompt and listen for 3 seconds
        touch_button_.OnClick([this]() {
            ESP_LOGI("CompactWifiBoard", "[MEDICATION] Button pressed");
            auto& app = Application::GetInstance();
            
            // Play medication prompt immediately
            app.PlayMedicationPrompt();
            
            // Start listening after prompt plays (about 1.5 seconds)
            app.GetBackgroundTask()->Schedule([this]() {
                vTaskDelay(pdMS_TO_TICKS(1500));  // Wait for prompt to play
                
                Application::GetInstance().Schedule([this]() {
                    ESP_LOGI("CompactWifiBoard", "[MEDICATION] Starting 3-second listening");
                    // Start normal listening with medication flag
                    Application::GetInstance().StartListening(true);
                    
                    // Stop after 3 seconds
                    Application::GetInstance().GetBackgroundTask()->Schedule([this]() {
                        vTaskDelay(pdMS_TO_TICKS(3000));  // Listen for 3 seconds
                        Application::GetInstance().Schedule([this]() {
                            ESP_LOGI("CompactWifiBoard", "[MEDICATION] Stopping after 3 seconds");
                            Application::GetInstance().StopListening();
                        });
                    });
                });
            });
        });

        volume_up_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_up_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_down_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
    }

    // 物联网初始化，逐步迁移到 MCP 协议
    void InitializeIot() {
#if CONFIG_IOT_PROTOCOL_XIAOZHI
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
        thing_manager.AddThing(iot::CreateThing("Lamp"));
#elif CONFIG_IOT_PROTOCOL_MCP
        static LampController lamp(LAMP_GPIO);
#endif
    }

    void InitializeSpeakerSD() {
#ifdef SPEAKER_SD_PIN
        gpio_config_t speaker_sd_config = {
            .pin_bit_mask = (1ULL << SPEAKER_SD_PIN),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        ESP_ERROR_CHECK(gpio_config(&speaker_sd_config));
        gpio_set_level(SPEAKER_SD_PIN, 1);  // SD is active low, so HIGH = enabled
        ESP_LOGI(TAG, "Speaker SD pin initialized on GPIO %d (HIGH=enabled)", SPEAKER_SD_PIN);
#endif
    }
    

    // Custom audio codec class with SD pin control
    class CompactWifiAudioCodec : public NoAudioCodecSimplex {
    public:
        CompactWifiAudioCodec(int input_sample_rate, int output_sample_rate,
            gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout,
            gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din)
            : NoAudioCodecSimplex(input_sample_rate, output_sample_rate,
                spk_bclk, spk_ws, spk_dout, mic_sck, mic_ws, mic_din) {
        }

        virtual void EnableOutput(bool enable) override {
            NoAudioCodecSimplex::EnableOutput(enable);
#ifdef SPEAKER_SD_PIN
            // Control MAX98357 SD pin (active low: LOW = shutdown, HIGH = enabled)
            gpio_set_level(SPEAKER_SD_PIN, enable ? 1 : 0);
            ESP_LOGI("CompactWifiAudioCodec", "Speaker SD pin set to %s (GPIO %d = %d)", 
                     enable ? "ENABLED" : "SHUTDOWN", SPEAKER_SD_PIN, enable ? 1 : 0);
#endif
        }
    };

public:
    CompactWifiBoard() :
        boot_button_(BOOT_BUTTON_GPIO),
        touch_button_(TOUCH_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        // Initialize displays based on mode
        if (use_dual_display_) {
            ESP_LOGI(TAG, "Initializing dual display mode with DualOledDisplay...");
            try {
                // Create dual display object
                display_ = new DualOledDisplay(DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                              DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
                                              {&font_puhui_14_1, &font_awesome_14_1});
                ESP_LOGI(TAG, "Dual display initialized successfully");
                
                // Set up the custom flush callback for mirroring
                lv_display_t* lvgl_display = lv_display_get_default();
                if (lvgl_display) {
                    lv_display_set_flush_cb(lvgl_display, DualOledDisplay::LvglFlushCallback);
                    ESP_LOGI(TAG, "Dual display mirroring enabled - both displays will show same content");
                }
                
#ifdef CONFIG_USE_EYE_ANIMATION_STYLE
                ESP_LOGI(TAG, "Eye animation style enabled for dual displays");
                ESP_LOGI(TAG, "Note: Eye animations designed for 240x240 will be cropped on 128x32 OLEDs");
#endif
            } catch (...) {
                ESP_LOGE(TAG, "Failed to initialize dual display, falling back to single display");
                use_dual_display_ = false;
                InitializeDisplayI2c();
                InitializeSsd1306Display();
                display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                         DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y,
                                         {&font_puhui_14_1, &font_awesome_14_1});
            }
        } else {
            // Single display mode
            InitializeDisplayI2c();
            InitializeSsd1306Display();
            display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y,
                                     {&font_puhui_14_1, &font_awesome_14_1});
        }
        InitializeButtons();
        InitializeIot();
        InitializeSpeakerSD();
        
        // Log microphone configuration
        if (use_dual_microphone_) {
            ESP_LOGI(TAG, "========================================");
            ESP_LOGI(TAG, "DUAL MICROPHONE MODE ENABLED");
            ESP_LOGI(TAG, "Mic 1: WS=GPIO%d, SCK=GPIO%d, SD=GPIO%d, L/R=GND", 
                     AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_DIN);
            ESP_LOGI(TAG, "Mic 2: WS=GPIO%d, SCK=GPIO%d, SD=GPIO%d, L/R=3.3V", 
                     AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_DIN);
            ESP_LOGI(TAG, "========================================");
        }
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static AudioCodec* audio_codec_instance = nullptr;
        static bool codec_initialized = false;
        
        if (!codec_initialized) {
            if (use_dual_microphone_) {
                // Use the new dual microphone codec
                static DualMicStereoCodec dual_audio_codec(
                    AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                    AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
                    AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN,
                    true);  // Enable dual mic mode
                audio_codec_instance = &dual_audio_codec;
                ESP_LOGI(TAG, "Initialized DUAL MICROPHONE audio codec");
            } else {
                // Use the original single microphone codec for backward compatibility
                static CompactWifiAudioCodec single_audio_codec(
                    AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                    AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
                    AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
                audio_codec_instance = &single_audio_codec;
                ESP_LOGI(TAG, "Initialized SINGLE MICROPHONE audio codec");
            }
            codec_initialized = true;
        }
        return audio_codec_instance;
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
        return &audio_codec;
#endif
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(CompactWifiBoard);
