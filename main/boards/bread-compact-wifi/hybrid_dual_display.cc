#include "hybrid_dual_display.h"
#include <string.h>
#include "gpio_test.h"

// Use GPIO 21 and 14 for software I2C (safe pins, no PSRAM conflict)
// Alternative pins to try if 21/14 don't work:
// - GPIO 1 and 2 (may affect boot if pulled low)
// - GPIO 11 and 12 (safe, not used)
// - GPIO 13 and 10 (safe, not used)

// Uncomment ONE of these options:
// #define USE_GPIO_1_2     // Try GPIO 1 (SDA) and 2 (SCL)
#define USE_GPIO_11_12   // Try GPIO 11 (SDA) and 12 (SCL)
// #define USE_GPIO_13_10   // Try GPIO 13 (SDA) and 10 (SCL)

#if defined(USE_GPIO_1_2)
#define SOFT_I2C_SDA_PIN GPIO_NUM_1
#define SOFT_I2C_SCL_PIN GPIO_NUM_2
#pragma message("Software I2C: Using GPIO 1/2 - may affect boot if pulled LOW")
#elif defined(USE_GPIO_11_12)
#define SOFT_I2C_SDA_PIN GPIO_NUM_11
#define SOFT_I2C_SCL_PIN GPIO_NUM_12
// GPIO 11/12 configuration (default)
#elif defined(USE_GPIO_13_10)
#define SOFT_I2C_SDA_PIN GPIO_NUM_13
#define SOFT_I2C_SCL_PIN GPIO_NUM_10
#pragma message("Software I2C: Using GPIO 13/10")
#else
// Default: GPIO 21/14
#define SOFT_I2C_SDA_PIN GPIO_NUM_21
#define SOFT_I2C_SCL_PIN GPIO_NUM_14
#endif

HybridDualDisplay::HybridDualDisplay() {
    // Calculate buffer size based on display dimensions
    buffer_size_ = (width_ * height_) / 8;  // 1 bit per pixel
    display1_buffer_ = new uint8_t[buffer_size_]();
    display2_buffer_ = new uint8_t[buffer_size_]();
    ESP_LOGI(TAG, "Created buffers of %d bytes for %dx%d displays", buffer_size_, width_, height_);
}

HybridDualDisplay::~HybridDualDisplay() {
    ESP_LOGI(TAG, "Cleaning up hybrid dual display resources");
    
    // Clean up display 1 (hardware I2C)
    if (panel1_) {
        esp_lcd_panel_del(panel1_);
    }
    if (hw_i2c_bus_) {
        i2c_del_master_bus(hw_i2c_bus_);
    }
    
    // Clean up display 2 (software I2C) - handled by unique_ptr
    
    // Clean up buffers
    delete[] display1_buffer_;
    delete[] display2_buffer_;
}

esp_err_t HybridDualDisplay::Initialize() {
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "Initializing Hybrid Dual Display System");
    ESP_LOGI(TAG, "Display 1: Hardware I2C on GPIO %d/%d", DISPLAY_SDA_PIN, DISPLAY_SCL_PIN);
    ESP_LOGI(TAG, "Display 2: Software I2C on GPIO %d/%d", SOFT_I2C_SDA_PIN, SOFT_I2C_SCL_PIN);
    ESP_LOGI(TAG, "===========================================");
    
    esp_err_t ret;
    
    // ========== Initialize Display 1 (Hardware I2C) ==========
    ESP_LOGI(TAG, "Setting up Display 1 with hardware I2C...");
    
    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = I2C_NUM_0;
    bus_config.sda_io_num = DISPLAY_SDA_PIN;
    bus_config.scl_io_num = DISPLAY_SCL_PIN;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.intr_priority = 0;
    bus_config.trans_queue_depth = 0;
    bus_config.flags.enable_internal_pullup = 1;
    
    ret = i2c_new_master_bus(&bus_config, &hw_i2c_bus_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create hardware I2C bus: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Hardware I2C bus initialized successfully");
    
    // Configure Display 1 panel IO
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
        .scl_speed_hz = 400 * 1000,  // 400kHz for hardware I2C
    };
    
    ret = esp_lcd_new_panel_io_i2c_v2(hw_i2c_bus_, &io_config, &panel1_io_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create panel IO for display 1: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Create SSD1306 panel for Display 1
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = -1;
    panel_config.bits_per_pixel = 1;
    
    esp_lcd_panel_ssd1306_config_t ssd1306_config = {
        .height = static_cast<uint8_t>(height_),
    };
    panel_config.vendor_config = &ssd1306_config;
    
    ret = esp_lcd_new_panel_ssd1306(panel1_io_, &panel_config, &panel1_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create panel 1: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Initialize Display 1
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel1_));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel1_));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel1_, true));
    ESP_LOGI(TAG, "✓ Display 1 initialized (Hardware I2C)");
    
    // ========== Initialize Display 2 (Software I2C) ==========
    ESP_LOGI(TAG, "Setting up Display 2 with software I2C...");
    ESP_LOGI(TAG, "Using safe GPIO pins that don't conflict with PSRAM");
    
    // Test GPIOs first
    GPIOTest::test_pins();
    
    // Create software I2C instance (try slower speed if issues)
    soft_i2c_ = std::make_unique<SoftwareI2C>(SOFT_I2C_SDA_PIN, SOFT_I2C_SCL_PIN, 50000);  // 50kHz (slower for reliability)
    
    // Scan for devices first
    soft_i2c_->scan_i2c();
    
    // Initialize SSD1306 on software I2C
    ret = soft_i2c_->ssd1306_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Display 2: %s", esp_err_to_name(ret));
        soft_i2c_.reset();
        // Continue with single display
        ESP_LOGW(TAG, "Continuing with Display 1 only");
    } else {
        ESP_LOGI(TAG, "✓ Display 2 initialized (Software I2C)");
    }
    
    // Clear both displays
    ClearBoth();
    
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "Hybrid dual display initialization complete!");
    ESP_LOGI(TAG, "Display 1: %s", IsDisplay1Ready() ? "READY" : "FAILED");
    ESP_LOGI(TAG, "Display 2: %s", IsDisplay2Ready() ? "READY" : "FAILED");
    ESP_LOGI(TAG, "===========================================");
    
    return ESP_OK;
}

esp_err_t HybridDualDisplay::UpdateDisplay1(const uint8_t* buffer, size_t len) {
    if (!panel1_ || !buffer) return ESP_ERR_INVALID_STATE;
    
    // Copy to internal buffer
    size_t copy_len = (len < buffer_size_) ? len : buffer_size_;
    memcpy(display1_buffer_, buffer, copy_len);
    
    // Update display using hardware I2C (fast)
    return esp_lcd_panel_draw_bitmap(panel1_, 0, 0, width_, height_, display1_buffer_);
}

esp_err_t HybridDualDisplay::UpdateDisplay2(const uint8_t* buffer, size_t len) {
    if (!soft_i2c_ || !buffer) return ESP_ERR_INVALID_STATE;
    
    ESP_LOGD(TAG, "Updating Display 2 with %d bytes", len);
    
    // Copy to internal buffer
    size_t copy_len = (len < buffer_size_) ? len : buffer_size_;
    memcpy(display2_buffer_, buffer, copy_len);
    
    // Send buffer to display via software I2C
    // Set addressing mode
    soft_i2c_->ssd1306_command(0x21);  // Column address
    soft_i2c_->ssd1306_command(0);     // Start column
    soft_i2c_->ssd1306_command(127);   // End column
    
    soft_i2c_->ssd1306_command(0x22);  // Page address
    soft_i2c_->ssd1306_command(0);     // Start page
    soft_i2c_->ssd1306_command((height_ / 8) - 1);  // End page
    
    // Send display data
    esp_err_t ret = soft_i2c_->write_data(0x3C, display2_buffer_, buffer_size_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update Display 2: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t HybridDualDisplay::ClearDisplay1() {
    if (!panel1_) return ESP_ERR_INVALID_STATE;
    memset(display1_buffer_, 0, buffer_size_);
    return esp_lcd_panel_draw_bitmap(panel1_, 0, 0, width_, height_, display1_buffer_);
}

esp_err_t HybridDualDisplay::ClearDisplay2() {
    if (!soft_i2c_) return ESP_ERR_INVALID_STATE;
    return soft_i2c_->ssd1306_clear();
}

esp_err_t HybridDualDisplay::ClearBoth() {
    ESP_LOGI(TAG, "Clearing both displays");
    ClearDisplay1();
    ClearDisplay2();
    return ESP_OK;
}

esp_err_t HybridDualDisplay::ShowTestPattern() {
    // Disabled - no longer showing test patterns
    ESP_LOGI(TAG, "Test patterns disabled");
    return ESP_OK;
}

esp_err_t HybridDualDisplay::TestBothDisplays() {
    ESP_LOGI(TAG, "=== TESTING BOTH DISPLAYS ===");
    
    // Test 1: All pixels on
    ESP_LOGI(TAG, "Test 1: All pixels ON");
    memset(display1_buffer_, 0xFF, buffer_size_);
    memset(display2_buffer_, 0xFF, buffer_size_);
    UpdateDisplay1(display1_buffer_, buffer_size_);
    UpdateDisplay2(display2_buffer_, buffer_size_);
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Test 2: Clear
    ESP_LOGI(TAG, "Test 2: Clear displays");
    ClearBoth();
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // Test 3: Test patterns
    ESP_LOGI(TAG, "Test 3: Test patterns");
    ShowTestPattern();
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Test 4: Inverse patterns
    ESP_LOGI(TAG, "Test 4: Inverse patterns");
    for (size_t i = 0; i < buffer_size_; i++) {
        display1_buffer_[i] = 0x55;  // 01010101
        display2_buffer_[i] = 0xF0;  // 11110000
    }
    UpdateDisplay1(display1_buffer_, buffer_size_);
    UpdateDisplay2(display2_buffer_, buffer_size_);
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    ESP_LOGI(TAG, "=== TEST COMPLETE ===");
    ClearBoth();
    
    return ESP_OK;
}