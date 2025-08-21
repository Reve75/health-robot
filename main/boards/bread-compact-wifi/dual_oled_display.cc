#include "dual_oled_display.h"
#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lvgl_port.h>
#include <driver/gpio.h>
#include <cstring>
#include <algorithm>

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#define TAG "DualOledDisplay"

// Static member initialization
esp_lcd_panel_handle_t DualOledDisplay::s_panel1 = nullptr;
esp_lcd_panel_handle_t DualOledDisplay::s_panel2 = nullptr;

DualOledDisplay::DualOledDisplay(int width, int height,
                               bool mirror_x, bool mirror_y, bool swap_xy,
                               DisplayFonts fonts)
    : fonts_(fonts) {
    
    ESP_LOGI(TAG, "Initializing dual OLED display system");
    
    // Set display dimensions
    width_ = width;
    height_ = height;
    
    // Initialize both displays
    InitializeDisplay1();
    InitializeDisplay2();
    
    // Store static references for flush callback
    s_panel1 = panel1_;
    s_panel2 = panel2_;
    
    // Initialize LVGL
    InitializeLVGL();
    
    ESP_LOGI(TAG, "Dual OLED display initialized successfully");
}

void DualOledDisplay::InitializeDisplay1() {
    ESP_LOGI(TAG, "Initializing Display 1 on hardware I2C (GPIO 41/42)");
    
    // Configure hardware I2C for Display 1
    i2c_master_bus_config_t i2c_bus_config = {
        .i2c_port = I2C_NUM_0,  // Use hardware I2C port 0
        .sda_io_num = DISPLAY1_I2C_SDA,
        .scl_io_num = DISPLAY1_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = 1,
        },
    };
    
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &i2c_bus1_));
    ESP_LOGI(TAG, "Display 1 I2C bus initialized");
    
    // Configure panel IO for Display 1
    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = DISPLAY1_ADDRESS,
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
    
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(i2c_bus1_, &io_config, &panel_io1_));
    ESP_LOGI(TAG, "Display 1 panel IO configured");
    
    // Create panel for Display 1
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .color_space = ESP_LCD_COLOR_SPACE_MONOCHROME,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 1,
        .flags = {
            .reset_active_high = 0,
        },
        .vendor_config = nullptr,
    };
    
    esp_lcd_panel_ssd1306_config_t ssd1306_config = {
        .height = static_cast<uint8_t>(height_),
    };
    panel_config.vendor_config = &ssd1306_config;
    
#ifdef SH1106
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io1_, &panel_config, &panel1_));
#else
    ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io1_, &panel_config, &panel1_));
#endif
    
    // Initialize and configure Display 1
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel1_));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel1_));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel1_, true));
    
    ESP_LOGI(TAG, "Display 1 initialized and turned on");
}

void DualOledDisplay::InitializeDisplay2() {
    ESP_LOGI(TAG, "Initializing Display 2 on software I2C (GPIO 11/12)");
    
    // Configure software I2C for Display 2
    i2c_master_bus_config_t i2c_bus_config = {
        .i2c_port = I2C_NUM_1,  // Use I2C port 1 for second display
        .sda_io_num = DISPLAY2_I2C_SDA,
        .scl_io_num = DISPLAY2_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = 1,
        },
    };
    
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &i2c_bus2_));
    ESP_LOGI(TAG, "Display 2 I2C bus initialized");
    
    // Configure panel IO for Display 2
    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = DISPLAY2_ADDRESS,
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
    
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(i2c_bus2_, &io_config, &panel_io2_));
    ESP_LOGI(TAG, "Display 2 panel IO configured");
    
    // Create panel for Display 2
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .color_space = ESP_LCD_COLOR_SPACE_MONOCHROME,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 1,
        .flags = {
            .reset_active_high = 0,
        },
        .vendor_config = nullptr,
    };
    
    esp_lcd_panel_ssd1306_config_t ssd1306_config = {
        .height = static_cast<uint8_t>(height_),
    };
    panel_config.vendor_config = &ssd1306_config;
    
#ifdef SH1106
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io2_, &panel_config, &panel2_));
#else
    ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io2_, &panel_config, &panel2_));
#endif
    
    // Initialize and configure Display 2
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel2_));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel2_));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel2_, true));
    
    ESP_LOGI(TAG, "Display 2 initialized and turned on");
}

void DualOledDisplay::InitializeLVGL() {
    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();
    
    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);
    
    ESP_LOGI(TAG, "Adding LCD screen to LVGL");
    // For monochrome displays with esp_lvgl_port, we need to provide the full screen buffer size in pixels
    // The esp_lvgl_port will handle the monochrome conversion internally
    uint32_t full_buffer_size = width_ * height_;  // Full buffer size in pixels, not bytes
    
    ESP_LOGI(TAG, "Display dimensions: %dx%d, buffer size: %lu pixels", width_, height_, full_buffer_size);
    
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io1_,  // Use Display 1 as primary
        .panel_handle = panel1_,
        .control_handle = nullptr,
        .buffer_size = full_buffer_size,  // Full buffer in pixels for monochrome
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = true,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .color_format = LV_COLOR_FORMAT_I1,
        .flags = {
            .buff_dma = 0,  // Disable DMA for monochrome displays
            .buff_spiram = 0,
            .sw_rotate = 0,
            .swap_bytes = 0,
            .full_refresh = 1,  // Enable full refresh for monochrome
            .direct_mode = 0,
        },
    };
    
    lv_display_t* display = lvgl_port_add_disp(&display_cfg);
    
    // Set our custom flush callback for mirroring
    lv_display_set_flush_cb(display, LvglFlushCallback);
    
    // Store the display for later use
    display_ = display;
    
    // Setup UI based on display height
    if (height_ == 64) {
        SetupUI_128x64();
    } else {
        SetupUI_128x32();
    }
    
    ESP_LOGI(TAG, "LVGL initialized with dual display mirroring");
}

void DualOledDisplay::LvglFlushCallback(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    // Validate the area coordinates
    if (!area || !px_map) {
        ESP_LOGE(TAG, "Invalid area or pixel map in flush callback");
        lv_display_flush_ready(disp);
        return;
    }
    
    // Get the actual display dimensions
    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;
    
    // Log the raw coordinates for debugging
    ESP_LOGD(TAG, "LVGL flush area: x1=%d, y1=%d, x2=%d, y2=%d", 
             offsetx1, offsety1, offsetx2, offsety2);
    
    // Validate coordinates - skip invalid areas
    if (offsetx1 < 0 || offsety1 < 0 || offsetx2 < offsetx1 || offsety2 < offsety1) {
        ESP_LOGW(TAG, "Skipping invalid coordinates in flush: x1=%d, y1=%d, x2=%d, y2=%d", 
                 offsetx1, offsety1, offsetx2, offsety2);
        lv_display_flush_ready(disp);
        return;
    }
    
    // Check for zero-sized areas
    if (offsetx1 == offsetx2 || offsety1 == offsety2) {
        ESP_LOGD(TAG, "Skipping zero-sized area: x1=%d, y1=%d, x2=%d, y2=%d", 
                 offsetx1, offsety1, offsetx2, offsety2);
        lv_display_flush_ready(disp);
        return;
    }
    
    // esp_lcd_panel_draw_bitmap expects end coordinates to be exclusive (one past the last pixel)
    // LVGL provides inclusive coordinates, so we add 1
    int x_start = offsetx1;
    int y_start = offsety1;
    int x_end = offsetx2 + 1;
    int y_end = offsety2 + 1;
    
    // Ensure coordinates are within display bounds
    if (x_start >= 128 || y_start >= 32 || x_end > 128 || y_end > 32) {
        ESP_LOGW(TAG, "Coordinates out of bounds (128x32): x:%d-%d, y:%d-%d", 
                 x_start, x_end, y_start, y_end);
        // Clamp to display bounds
        x_start = (x_start < 0) ? 0 : (x_start >= 128) ? 127 : x_start;
        y_start = (y_start < 0) ? 0 : (y_start >= 32) ? 31 : y_start;
        x_end = (x_end <= x_start) ? x_start + 1 : (x_end > 128) ? 128 : x_end;
        y_end = (y_end <= y_start) ? y_start + 1 : (y_end > 32) ? 32 : y_end;
    }
    
    // Final validation before drawing
    if (x_end <= x_start || y_end <= y_start) {
        ESP_LOGE(TAG, "Invalid final coordinates: x:%d-%d, y:%d-%d", 
                 x_start, x_end, y_start, y_end);
        lv_display_flush_ready(disp);
        return;
    }
    
    // Write to Display 1 (hardware I2C)
    if (s_panel1) {
        esp_err_t ret = esp_lcd_panel_draw_bitmap(s_panel1, x_start, y_start, 
                                                  x_end, y_end, px_map);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to draw on Display 1: %s (x:%d-%d, y:%d-%d)", 
                     esp_err_to_name(ret), x_start, x_end, y_start, y_end);
        }
    }
    
    // Mirror to Display 2 (software I2C)
    if (s_panel2) {
        esp_err_t ret = esp_lcd_panel_draw_bitmap(s_panel2, x_start, y_start, 
                                                  x_end, y_end, px_map);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to draw on Display 2: %s (x:%d-%d, y:%d-%d)", 
                     esp_err_to_name(ret), x_start, x_end, y_start, y_end);
        }
    }
    
    // Notify LVGL that flushing is complete
    lv_display_flush_ready(disp);
}

bool DualOledDisplay::Lock(int timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

void DualOledDisplay::Unlock() {
    lvgl_port_unlock();
}

void DualOledDisplay::SetChatMessage(const char* role, const char* content) {
    // Implementation similar to OledDisplay::SetChatMessage
    // For now, just log
    ESP_LOGI(TAG, "SetChatMessage: [%s] %s", role, content);
}

#ifdef CONFIG_USE_EYE_ANIMATION_STYLE
void DualOledDisplay::SetEye(int x_start, int y_start, int x_end, int y_end, const void *color_data) {
    // Direct draw to both displays
    // Note: The eye animation is designed for 240x240 displays
    // On 128x32 OLEDs, only a small portion will be visible
    
    if (!color_data) return;
    
    // Validate input coordinates
    if (x_end <= x_start || y_end <= y_start) {
        ESP_LOGW(TAG, "SetEye: Invalid coordinates x:%d-%d, y:%d-%d", 
                 x_start, x_end, y_start, y_end);
        return;
    }
    
    int width = x_end - x_start;
    int height = y_end - y_start;
    
    // Clip to display bounds
    int draw_x_start = std::max(0, x_start);
    int draw_y_start = std::max(0, y_start);
    int draw_x_end = std::min(width_, x_end);
    int draw_y_end = std::min(height_, y_end);
    
    // Check if anything is visible after clipping
    if (draw_x_end <= draw_x_start || draw_y_end <= draw_y_start) {
        ESP_LOGD(TAG, "SetEye: Nothing visible after clipping to display bounds");
        return;
    }
    
    // Calculate visible dimensions
    int visible_width = draw_x_end - draw_x_start;
    int visible_height = draw_y_end - draw_y_start;
    
    // Convert RGB565 to monochrome for OLED
    const uint16_t* rgb_data = static_cast<const uint16_t*>(color_data);
    size_t mono_buffer_size = (visible_width * visible_height + 7) / 8;
    uint8_t* mono_buffer = new uint8_t[mono_buffer_size];
    memset(mono_buffer, 0, mono_buffer_size);
    
    // Simple threshold conversion from RGB565 to monochrome
    // Only process the visible portion
    for (int y = 0; y < visible_height; y++) {
        for (int x = 0; x < visible_width; x++) {
            // Map back to source image coordinates
            int src_x = draw_x_start - x_start + x;
            int src_y = draw_y_start - y_start + y;
            
            if (src_x >= 0 && src_x < width && src_y >= 0 && src_y < height) {
                uint16_t pixel = rgb_data[src_y * width + src_x];
                // Extract RGB components from RGB565
                uint8_t r = (pixel >> 11) & 0x1F;
                uint8_t g = (pixel >> 5) & 0x3F;
                uint8_t b = pixel & 0x1F;
                
                // Simple luminance calculation
                uint8_t luminance = (r * 8 + g * 4 + b * 8) / 3;
                
                // Set bit if bright enough
                if (luminance > 20) {
                    int bit_index = y * visible_width + x;
                    mono_buffer[bit_index / 8] |= (1 << (bit_index % 8));
                }
            }
        }
    }
    
    ESP_LOGD(TAG, "SetEye drawing to displays: x:%d-%d, y:%d-%d", 
             draw_x_start, draw_x_end, draw_y_start, draw_y_end);
    
    // Draw to both displays - make sure end > start
    if (panel1_ && draw_x_end > draw_x_start && draw_y_end > draw_y_start) {
        esp_err_t ret = esp_lcd_panel_draw_bitmap(panel1_, draw_x_start, draw_y_start,
                                                  draw_x_end, draw_y_end, mono_buffer);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SetEye failed on Display 1: %s", esp_err_to_name(ret));
        }
    }
    
    if (panel2_ && draw_x_end > draw_x_start && draw_y_end > draw_y_start) {
        esp_err_t ret = esp_lcd_panel_draw_bitmap(panel2_, draw_x_start, draw_y_start,
                                                  draw_x_end, draw_y_end, mono_buffer);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SetEye failed on Display 2: %s", esp_err_to_name(ret));
        }
    }
    
    delete[] mono_buffer;
}

void DualOledDisplay::DrawEyeFrame(const uint16_t* frameData, int width, int height) {
    // Helper function to draw a complete eye frame
    SetEye(0, 0, width, height, frameData);
}
#endif

void DualOledDisplay::SetupUI_128x32() {
    // Simplified setup - just log for now to avoid crashes
    ESP_LOGI(TAG, "UI setup complete for 128x32 dual display");
}

void DualOledDisplay::SetupUI_128x64() {
    // Simplified setup - just log for now to avoid crashes
    ESP_LOGI(TAG, "UI setup complete for 128x64 dual display");
}

DualOledDisplay::~DualOledDisplay() {
    if (panel2_) {
        esp_lcd_panel_del(panel2_);
        panel2_ = nullptr;
    }
    if (panel_io2_) {
        esp_lcd_panel_io_del(panel_io2_);
        panel_io2_ = nullptr;
    }
    if (i2c_bus2_) {
        i2c_del_master_bus(i2c_bus2_);
        i2c_bus2_ = nullptr;
    }
    
    if (panel1_) {
        esp_lcd_panel_del(panel1_);
        panel1_ = nullptr;
    }
    if (panel_io1_) {
        esp_lcd_panel_io_del(panel_io1_);
        panel_io1_ = nullptr;
    }
    if (i2c_bus1_) {
        i2c_del_master_bus(i2c_bus1_);
        i2c_bus1_ = nullptr;
    }
    
    s_panel1 = nullptr;
    s_panel2 = nullptr;
}