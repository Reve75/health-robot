#pragma once

#include <driver/i2c_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <memory>
#include "software_i2c.h"
#include "config.h"

// Hybrid dual display manager using hardware I2C for display 1 and software I2C for display 2
// This avoids GPIO 37/38 PSRAM conflicts completely
class HybridDualDisplay {
private:
    static constexpr const char* TAG = "HybridDisplay";
    
    // Display 1: Hardware I2C (fast, primary display)
    i2c_master_bus_handle_t hw_i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel1_io_ = nullptr;
    esp_lcd_panel_handle_t panel1_ = nullptr;
    
    // Display 2: Software I2C (slower, secondary display)
    std::unique_ptr<SoftwareI2C> soft_i2c_;
    
    // Display buffers
    uint8_t* display1_buffer_ = nullptr;
    uint8_t* display2_buffer_ = nullptr;
    size_t buffer_size_;
    
    // Display dimensions
    int width_ = 128;
    int height_ = DISPLAY_HEIGHT;
    
public:
    HybridDualDisplay();
    ~HybridDualDisplay();
    
    // Initialize both displays
    esp_err_t Initialize();
    
    // Display 1 operations (hardware I2C - fast)
    esp_err_t UpdateDisplay1(const uint8_t* buffer, size_t len);
    esp_err_t ClearDisplay1();
    esp_err_t Display1ShowText(const char* text);
    
    // Display 2 operations (software I2C - slower)
    esp_err_t UpdateDisplay2(const uint8_t* buffer, size_t len);
    esp_err_t ClearDisplay2();
    esp_err_t Display2ShowText(const char* text);
    
    // Both displays
    esp_err_t ClearBoth();
    esp_err_t ShowTestPattern();
    esp_err_t TestBothDisplays();
    
    // Status
    bool IsDisplay1Ready() const { return panel1_ != nullptr; }
    bool IsDisplay2Ready() const { return soft_i2c_ != nullptr; }
    
    // Get the hardware I2C panel and IO for use with OledDisplay
    esp_lcd_panel_handle_t GetPanel1() const { return panel1_; }
    esp_lcd_panel_io_handle_t GetPanelIO1() const { return panel1_io_; }
    
    // Get dimensions
    int GetWidth() const { return width_; }
    int GetHeight() const { return height_; }
};