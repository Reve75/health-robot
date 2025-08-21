#pragma once

#include <driver/i2c_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <cstring>
#include <memory>

class DualDisplayManager {
private:
    static constexpr const char* TAG = "DualDisplay";
    
    // I2C bus handles
    i2c_master_bus_handle_t i2c_bus0_ = nullptr;  // Display 1 bus
    i2c_master_bus_handle_t i2c_bus1_ = nullptr;  // Display 2 bus
    
    // Panel handles
    esp_lcd_panel_handle_t panel1_ = nullptr;
    esp_lcd_panel_handle_t panel2_ = nullptr;
    
    // Panel IO handles
    esp_lcd_panel_io_handle_t io_handle1_ = nullptr;
    esp_lcd_panel_io_handle_t io_handle2_ = nullptr;
    
    // Display buffers
    uint8_t* display1_buffer_ = nullptr;
    uint8_t* display2_buffer_ = nullptr;
    size_t buffer_size_;
    
    // Helper function to scan I2C buses for debugging
    void ScanI2CBuses();
    
public:
    DualDisplayManager();
    ~DualDisplayManager();
    
    // Initialize both displays
    esp_err_t Initialize();
    
    // Update display content
    void UpdateDisplay1(const uint8_t* buffer, size_t len);
    void UpdateDisplay2(const uint8_t* buffer, size_t len);
    
    // Clear displays
    void ClearDisplay1();
    void ClearDisplay2();
    void ClearBoth();
    
    // Test patterns for verification
    void ShowTestPattern();
    
    // Get display dimensions
    int GetWidth() const { return 128; }
    int GetHeight() const;
};