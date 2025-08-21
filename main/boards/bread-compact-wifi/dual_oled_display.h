#ifndef DUAL_OLED_DISPLAY_H
#define DUAL_OLED_DISPLAY_H

#include "display/oled_display.h"
#include <esp_lcd_panel_ops.h>
#include <driver/i2c_master.h>
#include <cstring>
#include <vector>

// Pin definitions for dual displays
#define DISPLAY1_I2C_SDA  GPIO_NUM_41
#define DISPLAY1_I2C_SCL  GPIO_NUM_42
#define DISPLAY1_ADDRESS  0x3C

#define DISPLAY2_I2C_SDA  GPIO_NUM_11
#define DISPLAY2_I2C_SCL  GPIO_NUM_12
#define DISPLAY2_ADDRESS  0x3C

/**
 * DualOledDisplay - Manages two OLED displays with mirroring
 * Based on RoPet's DualScreenDisplay pattern but adapted for I2C OLEDs
 */
class DualOledDisplay : public Display {
protected:
    esp_lcd_panel_io_handle_t panel_io1_ = nullptr;
    esp_lcd_panel_handle_t panel1_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io2_ = nullptr;
    esp_lcd_panel_handle_t panel2_ = nullptr;
    i2c_master_bus_handle_t i2c_bus1_;  // Hardware I2C bus for display 1
    i2c_master_bus_handle_t i2c_bus2_;  // Software I2C bus for display 2
    
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* content_left_ = nullptr;
    lv_obj_t* content_right_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    
    DisplayFonts fonts_;
    
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;
    
    void SetupUI_128x64();
    void SetupUI_128x32();
    
private:
    
    // Static callback needs access to both panels
    static esp_lcd_panel_handle_t s_panel1;
    static esp_lcd_panel_handle_t s_panel2;
    
    // Custom flush callback for dual display mirroring
    static bool DualDisplayFlushCallback(esp_lcd_panel_io_handle_t panel_io, 
                                         esp_lcd_panel_io_event_data_t *edata, 
                                         void *user_ctx);
    
    void InitializeDisplay1();
    void InitializeDisplay2();
    void InitializeLVGL();
    
public:
    DualOledDisplay(int width, int height, 
                   bool mirror_x, bool mirror_y, bool swap_xy,
                   DisplayFonts fonts);
    
    virtual ~DualOledDisplay();
    
    virtual void SetChatMessage(const char* role, const char* content) override;
    
#ifdef CONFIG_USE_EYE_ANIMATION_STYLE
    // Eye animation support
    virtual void SetEye(int x_start, int y_start, int x_end, int y_end, const void *color_data) override;
    void DrawEyeFrame(const uint16_t* frameData, int width, int height);
#endif
    
    // Custom LVGL flush callback to mirror to both displays
    static void LvglFlushCallback(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map);
};

#endif // DUAL_OLED_DISPLAY_H