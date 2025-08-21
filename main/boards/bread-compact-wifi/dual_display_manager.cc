#include "dual_display_manager.h"
#include "config.h"
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

DualDisplayManager::DualDisplayManager() {
    // Calculate buffer size based on display dimensions
    buffer_size_ = (128 * DISPLAY_HEIGHT) / 8;  // 1 bit per pixel
    display1_buffer_ = new uint8_t[buffer_size_]();
    display2_buffer_ = new uint8_t[buffer_size_]();
}

DualDisplayManager::~DualDisplayManager() {
    ESP_LOGI(TAG, "Cleaning up dual display resources");
    
    // Clean up displays
    if (panel1_) {
        esp_lcd_panel_del(panel1_);
    }
    if (panel2_) {
        esp_lcd_panel_del(panel2_);
    }
    
    // IO handles are automatically cleaned up with panels in ESP-IDF v5.3
    
    // Clean up I2C buses
    if (i2c_bus0_) {
        i2c_del_master_bus(i2c_bus0_);
    }
    if (i2c_bus1_) {
        i2c_del_master_bus(i2c_bus1_);
    }
    
    // Clean up buffers
    delete[] display1_buffer_;
    delete[] display2_buffer_;
}

void DualDisplayManager::ScanI2CBuses() {
    ESP_LOGI(TAG, "Scanning I2C Bus 0...");
    for (uint8_t addr = 0x03; addr < 0x78; addr++) {
        i2c_master_dev_handle_t dev_handle;
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = 100000,
        };
        
        if (i2c_master_bus_add_device(i2c_bus0_, &dev_cfg, &dev_handle) == ESP_OK) {
            uint8_t test_data;
            if (i2c_master_transmit_receive(dev_handle, nullptr, 0, &test_data, 1, 100) == ESP_OK) {
                ESP_LOGI(TAG, "  Found device on Bus 0 at address 0x%02X", addr);
            }
            i2c_master_bus_rm_device(dev_handle);
        }
    }
    
    ESP_LOGI(TAG, "Scanning I2C Bus 1...");
    for (uint8_t addr = 0x03; addr < 0x78; addr++) {
        i2c_master_dev_handle_t dev_handle;
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = 100000,
        };
        
        if (i2c_master_bus_add_device(i2c_bus1_, &dev_cfg, &dev_handle) == ESP_OK) {
            uint8_t test_data;
            if (i2c_master_transmit_receive(dev_handle, nullptr, 0, &test_data, 1, 100) == ESP_OK) {
                ESP_LOGI(TAG, "  Found device on Bus 1 at address 0x%02X", addr);
            }
            i2c_master_bus_rm_device(dev_handle);
        }
    }
}

esp_err_t DualDisplayManager::Initialize() {
    ESP_LOGI(TAG, "Initializing dual displays with separate I2C buses");
    ESP_LOGI(TAG, "Display 1: I2C0 on GPIO %d/%d", DISPLAY_SDA_PIN, DISPLAY_SCL_PIN);
    ESP_LOGI(TAG, "Display 2: I2C1 on GPIO %d/%d", DISPLAY2_SDA_PIN, DISPLAY2_SCL_PIN);
    
    // ========== Initialize I2C Bus 0 for Display 1 ==========
    i2c_master_bus_config_t bus0_config = {};
    bus0_config.i2c_port = I2C_NUM_0;
    bus0_config.sda_io_num = DISPLAY_SDA_PIN;
    bus0_config.scl_io_num = DISPLAY_SCL_PIN;
    bus0_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus0_config.glitch_ignore_cnt = 7;
    bus0_config.intr_priority = 0;
    bus0_config.trans_queue_depth = 0;
    bus0_config.flags.enable_internal_pullup = 1;
    
    esp_err_t ret = i2c_new_master_bus(&bus0_config, &i2c_bus0_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C bus 0: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "I2C Bus 0 initialized successfully");
    
    // ========== Initialize I2C Bus 1 for Display 2 ==========
    ESP_LOGI(TAG, "Attempting to initialize I2C Bus 1 with GPIO %d (SDA) and GPIO %d (SCL)", DISPLAY2_SDA_PIN, DISPLAY2_SCL_PIN);
    
    // Check if GPIOs are valid for ESP32-S3
    if (DISPLAY2_SDA_PIN >= GPIO_NUM_MAX || DISPLAY2_SCL_PIN >= GPIO_NUM_MAX) {
        ESP_LOGE(TAG, "Invalid GPIO pins for I2C Bus 1: SDA=%d, SCL=%d", DISPLAY2_SDA_PIN, DISPLAY2_SCL_PIN);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Try with lower speed first to avoid timing issues
    i2c_master_bus_config_t bus1_config = {};
    bus1_config.i2c_port = I2C_NUM_1;
    bus1_config.sda_io_num = DISPLAY2_SDA_PIN;
    bus1_config.scl_io_num = DISPLAY2_SCL_PIN;
    bus1_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus1_config.glitch_ignore_cnt = 7;
    bus1_config.intr_priority = 0;
    bus1_config.trans_queue_depth = 0;
    bus1_config.flags.enable_internal_pullup = 1;
    
    ret = i2c_new_master_bus(&bus1_config, &i2c_bus1_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C bus 1: %s (error code: 0x%x)", esp_err_to_name(ret), ret);
        ESP_LOGE(TAG, "Check if GPIO %d and GPIO %d are available and not used elsewhere", DISPLAY2_SDA_PIN, DISPLAY2_SCL_PIN);
        ESP_LOGE(TAG, "Possible causes: pins already in use, hardware issue, or missing pull-up resistors");
        
        // Clean up bus 0 before returning
        if (i2c_bus0_) {
            i2c_del_master_bus(i2c_bus0_);
            i2c_bus0_ = nullptr;
        }
        return ret;
    }
    ESP_LOGI(TAG, "I2C Bus 1 initialized successfully on GPIO %d/%d", DISPLAY2_SDA_PIN, DISPLAY2_SCL_PIN);
    
    // ========== Configure Display 1 (Bus 0, Address 0x3C) ==========
    esp_lcd_panel_io_i2c_config_t io_config1 = {
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
        .scl_speed_hz = 400 * 1000,  // 400kHz
    };
    
    ret = esp_lcd_new_panel_io_i2c_v2(i2c_bus0_, &io_config1, &io_handle1_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create panel IO for display 1: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // ========== Configure Display 2 (Bus 1, Address 0x3C) ==========
    esp_lcd_panel_io_i2c_config_t io_config2 = {
        .dev_addr = DISPLAY2_I2C_ADDR,  // Also 0x3C, but on different bus
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
        .scl_speed_hz = 400 * 1000,  // 400kHz
    };
    
    ret = esp_lcd_new_panel_io_i2c_v2(i2c_bus1_, &io_config2, &io_handle2_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create panel IO for display 2: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // ========== Create SSD1306 Panel for Display 1 ==========
    esp_lcd_panel_dev_config_t panel_config1 = {};
    panel_config1.reset_gpio_num = -1;
    panel_config1.bits_per_pixel = 1;
    
    esp_lcd_panel_ssd1306_config_t ssd1306_config1 = {
        .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
    };
    panel_config1.vendor_config = &ssd1306_config1;
    
    ESP_LOGI(TAG, "Creating SSD1306 panel for Display 1 (height=%d)", DISPLAY_HEIGHT);
    ret = esp_lcd_new_panel_ssd1306(io_handle1_, &panel_config1, &panel1_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create panel 1: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Initialize Display 1
    ESP_LOGI(TAG, "Resetting Display 1...");
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel1_));
    ESP_LOGI(TAG, "Initializing Display 1...");
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel1_));
    ESP_LOGI(TAG, "Turning on Display 1...");
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel1_, true));
    
    // Immediately draw test pattern to verify display 1 is working
    ESP_LOGI(TAG, "Testing Display 1 with immediate pattern...");
    uint8_t test1[512];
    memset(test1, 0xFF, sizeof(test1));  // All pixels on
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel1_, 0, 0, 128, DISPLAY_HEIGHT, test1));
    vTaskDelay(pdMS_TO_TICKS(500));
    
    ESP_LOGI(TAG, "Display 1 initialized and turned on");
    
    // ========== Create SSD1306 Panel for Display 2 ==========
    esp_lcd_panel_dev_config_t panel_config2 = {};
    panel_config2.reset_gpio_num = -1;
    panel_config2.bits_per_pixel = 1;
    
    esp_lcd_panel_ssd1306_config_t ssd1306_config2 = {
        .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
    };
    panel_config2.vendor_config = &ssd1306_config2;
    
    ESP_LOGI(TAG, "Creating SSD1306 panel for Display 2 (height=%d)", DISPLAY_HEIGHT);
    ret = esp_lcd_new_panel_ssd1306(io_handle2_, &panel_config2, &panel2_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create panel 2: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Initialize Display 2
    ESP_LOGI(TAG, "Resetting Display 2...");
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel2_));
    ESP_LOGI(TAG, "Initializing Display 2...");
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel2_));
    ESP_LOGI(TAG, "Turning on Display 2...");
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel2_, true));
    
    // Immediately draw test pattern to verify display 2 is working
    ESP_LOGI(TAG, "Testing Display 2 with immediate pattern...");
    uint8_t test2[512];
    memset(test2, 0x55, sizeof(test2));  // Different pattern
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel2_, 0, 0, 128, DISPLAY_HEIGHT, test2));
    vTaskDelay(pdMS_TO_TICKS(500));
    
    ESP_LOGI(TAG, "Display 2 initialized and turned on");
    
    // Scan I2C buses to verify devices are present
    ScanI2CBuses();
    
    // Clear both displays
    ClearBoth();
    
    ESP_LOGI(TAG, "Dual display initialization complete!");
    return ESP_OK;
}

void DualDisplayManager::UpdateDisplay1(const uint8_t* buffer, size_t len) {
    if (!panel1_ || !buffer) return;
    
    // Copy to internal buffer
    size_t copy_len = (len < buffer_size_) ? len : buffer_size_;
    memcpy(display1_buffer_, buffer, copy_len);
    
    // Update display
    esp_lcd_panel_draw_bitmap(panel1_, 0, 0, 128, DISPLAY_HEIGHT, display1_buffer_);
}

void DualDisplayManager::UpdateDisplay2(const uint8_t* buffer, size_t len) {
    if (!panel2_ || !buffer) return;
    
    // Copy to internal buffer
    size_t copy_len = (len < buffer_size_) ? len : buffer_size_;
    memcpy(display2_buffer_, buffer, copy_len);
    
    // Update display
    esp_lcd_panel_draw_bitmap(panel2_, 0, 0, 128, DISPLAY_HEIGHT, display2_buffer_);
}

void DualDisplayManager::ClearDisplay1() {
    if (!panel1_) return;
    memset(display1_buffer_, 0, buffer_size_);
    esp_lcd_panel_draw_bitmap(panel1_, 0, 0, 128, DISPLAY_HEIGHT, display1_buffer_);
}

void DualDisplayManager::ClearDisplay2() {
    if (!panel2_) return;
    memset(display2_buffer_, 0, buffer_size_);
    esp_lcd_panel_draw_bitmap(panel2_, 0, 0, 128, DISPLAY_HEIGHT, display2_buffer_);
}

void DualDisplayManager::ClearBoth() {
    ClearDisplay1();
    ClearDisplay2();
    ESP_LOGI(TAG, "Both displays cleared");
}

void DualDisplayManager::ShowTestPattern() {
    ESP_LOGI(TAG, "Showing test pattern on both displays");
    
    // Create different patterns for each display
    // Display 1: Vertical stripes
    for (size_t i = 0; i < buffer_size_; i++) {
        display1_buffer_[i] = 0xAA;  // 10101010 pattern
    }
    
    // Display 2: Checkerboard pattern
    for (size_t row = 0; row < DISPLAY_HEIGHT / 8; row++) {
        for (size_t col = 0; col < 128; col++) {
            // Create checkerboard: alternate every 8 pixels
            uint8_t pattern = ((col / 8) % 2) ^ ((row % 2)) ? 0xFF : 0x00;
            display2_buffer_[row * 128 + col] = pattern;
        }
    }
    
    // Update both displays
    ESP_LOGI(TAG, "Updating Display 1 with vertical stripes...");
    esp_err_t ret = esp_lcd_panel_draw_bitmap(panel1_, 0, 0, 128, DISPLAY_HEIGHT, display1_buffer_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update Display 1: %s", esp_err_to_name(ret));
    }
    
    ESP_LOGI(TAG, "Updating Display 2 with checkerboard pattern...");
    ret = esp_lcd_panel_draw_bitmap(panel2_, 0, 0, 128, DISPLAY_HEIGHT, display2_buffer_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update Display 2: %s", esp_err_to_name(ret));
    }
    
    ESP_LOGI(TAG, "Test pattern displayed - Display 1: vertical stripes, Display 2: checkerboard");
}

int DualDisplayManager::GetHeight() const {
    return DISPLAY_HEIGHT;
}