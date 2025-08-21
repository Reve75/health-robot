#pragma once

#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Software I2C implementation for second display
// This avoids GPIO 37/38 PSRAM conflict
class SoftwareI2C {
private:
    static constexpr const char* TAG = "SoftI2C";
    gpio_num_t sda_pin_;
    gpio_num_t scl_pin_;
    int delay_us_;  // Microseconds delay for bit timing
    
    void set_sda(bool state);
    void set_scl(bool state);
    bool read_sda();
    void i2c_delay();
    
public:
    SoftwareI2C(gpio_num_t sda_pin, gpio_num_t scl_pin, uint32_t frequency = 100000);
    ~SoftwareI2C();
    
    // I2C operations
    void start();
    void stop();
    bool write_byte(uint8_t byte);
    uint8_t read_byte(bool ack);
    
    // High-level operations
    esp_err_t write_command(uint8_t addr, uint8_t cmd);
    esp_err_t write_data(uint8_t addr, const uint8_t* data, size_t len);
    esp_err_t write_cmd_data(uint8_t addr, uint8_t cmd, const uint8_t* data, size_t len);
    
    // SSD1306 specific
    esp_err_t ssd1306_command(uint8_t cmd);
    esp_err_t ssd1306_data(const uint8_t* data, size_t len);
    esp_err_t ssd1306_init();
    esp_err_t ssd1306_set_pos(uint8_t x, uint8_t y);
    esp_err_t ssd1306_clear();
    esp_err_t ssd1306_display_on(bool on);
    
    // Debug functions
    void scan_i2c();
    bool probe_address(uint8_t addr);
};