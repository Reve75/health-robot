#include "software_i2c.h"
#include <string.h>
#include <esp_rom_sys.h>

// SSD1306 I2C address (7-bit)
#define SSD1306_ADDR 0x3C

// SSD1306 commands
#define SSD1306_CMD_DISPLAY_OFF         0xAE
#define SSD1306_CMD_DISPLAY_ON          0xAF
#define SSD1306_CMD_SET_DISPLAY_CLK_DIV 0xD5
#define SSD1306_CMD_SET_MULTIPLEX       0xA8
#define SSD1306_CMD_SET_DISPLAY_OFFSET  0xD3
#define SSD1306_CMD_SET_START_LINE      0x40
#define SSD1306_CMD_CHARGE_PUMP         0x8D
#define SSD1306_CMD_MEM_ADDR_MODE       0x20
#define SSD1306_CMD_SEG_REMAP           0xA0
#define SSD1306_CMD_COM_SCAN_DIR        0xC0
#define SSD1306_CMD_SET_COM_PINS        0xDA
#define SSD1306_CMD_SET_CONTRAST        0x81
#define SSD1306_CMD_SET_PRECHARGE       0xD9
#define SSD1306_CMD_SET_VCOM_DESEL      0xDB
#define SSD1306_CMD_DISPLAY_ALL_ON      0xA4
#define SSD1306_CMD_NORMAL_DISPLAY      0xA6
#define SSD1306_CMD_COLUMN_ADDR         0x21
#define SSD1306_CMD_PAGE_ADDR           0x22

SoftwareI2C::SoftwareI2C(gpio_num_t sda_pin, gpio_num_t scl_pin, uint32_t frequency) 
    : sda_pin_(sda_pin), scl_pin_(scl_pin) {
    
    // Calculate delay based on frequency (default 100kHz)
    delay_us_ = 1000000 / (frequency * 2);
    if (delay_us_ < 1) delay_us_ = 1;
    
    ESP_LOGI(TAG, "Initializing software I2C on SDA=%d, SCL=%d, freq=%lu Hz, delay=%d us", 
             sda_pin_, scl_pin_, (unsigned long)frequency, delay_us_);
    
    // Configure GPIO pins - try push-pull mode first for stronger signals
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;  // Push-pull for stronger signal
    io_conf.pin_bit_mask = (1ULL << sda_pin_) | (1ULL << scl_pin_);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;  // No pull-ups in push-pull mode
    gpio_config(&io_conf);
    
    // Set initial high state (idle)
    gpio_set_level(sda_pin_, 1);
    gpio_set_level(scl_pin_, 1);
    
    // Small delay to let lines stabilize
    vTaskDelay(pdMS_TO_TICKS(1));
}

SoftwareI2C::~SoftwareI2C() {
    // Return pins to high impedance state
    gpio_reset_pin(sda_pin_);
    gpio_reset_pin(scl_pin_);
}

void SoftwareI2C::i2c_delay() {
    esp_rom_delay_us(delay_us_);
}

void SoftwareI2C::set_sda(bool state) {
    gpio_set_level(sda_pin_, state ? 1 : 0);
}

void SoftwareI2C::set_scl(bool state) {
    gpio_set_level(scl_pin_, state ? 1 : 0);
}

bool SoftwareI2C::read_sda() {
    // Temporarily switch to input mode to read
    gpio_set_direction(sda_pin_, GPIO_MODE_INPUT);
    gpio_set_pull_mode(sda_pin_, GPIO_PULLUP_ONLY);
    bool val = gpio_get_level(sda_pin_);
    // Switch back to output
    gpio_set_direction(sda_pin_, GPIO_MODE_OUTPUT);
    return val;
}

void SoftwareI2C::start() {
    // I2C Start condition: SDA goes low while SCL is high
    set_sda(1);
    set_scl(1);
    i2c_delay();
    set_sda(0);
    i2c_delay();
    set_scl(0);
    i2c_delay();
}

void SoftwareI2C::stop() {
    // I2C Stop condition: SDA goes high while SCL is high
    set_sda(0);
    set_scl(0);
    i2c_delay();
    set_scl(1);
    i2c_delay();
    set_sda(1);
    i2c_delay();
}

bool SoftwareI2C::write_byte(uint8_t byte) {
    // Write 8 bits
    for (int i = 7; i >= 0; i--) {
        set_sda((byte >> i) & 1);
        i2c_delay();
        set_scl(1);
        i2c_delay();
        set_scl(0);
        i2c_delay();
    }
    
    // Read ACK bit - release SDA line by switching to input
    gpio_set_direction(sda_pin_, GPIO_MODE_INPUT);
    gpio_set_pull_mode(sda_pin_, GPIO_PULLUP_ONLY);
    i2c_delay();
    set_scl(1);
    i2c_delay();
    bool ack = !gpio_get_level(sda_pin_);  // ACK is low
    set_scl(0);
    i2c_delay();
    // Switch back to output
    gpio_set_direction(sda_pin_, GPIO_MODE_OUTPUT);
    
    return ack;
}

uint8_t SoftwareI2C::read_byte(bool ack) {
    uint8_t byte = 0;
    
    // Read 8 bits - switch to input mode
    gpio_set_direction(sda_pin_, GPIO_MODE_INPUT);
    gpio_set_pull_mode(sda_pin_, GPIO_PULLUP_ONLY);
    
    for (int i = 7; i >= 0; i--) {
        i2c_delay();
        set_scl(1);
        i2c_delay();
        if (gpio_get_level(sda_pin_)) {
            byte |= (1 << i);
        }
        set_scl(0);
    }
    
    // Switch back to output to send ACK/NACK
    gpio_set_direction(sda_pin_, GPIO_MODE_OUTPUT);
    set_sda(ack ? 0 : 1);
    i2c_delay();
    set_scl(1);
    i2c_delay();
    set_scl(0);
    set_sda(1);
    i2c_delay();
    
    return byte;
}

esp_err_t SoftwareI2C::write_command(uint8_t addr, uint8_t cmd) {
    start();
    
    // Send address with write bit
    if (!write_byte((addr << 1) | 0)) {
        stop();
        ESP_LOGE(TAG, "No ACK for address 0x%02X", addr);
        return ESP_ERR_NOT_FOUND;
    }
    
    // Send control byte (Co=0, D/C#=0 for command)
    if (!write_byte(0x00)) {
        stop();
        return ESP_ERR_INVALID_RESPONSE;
    }
    
    // Send command
    if (!write_byte(cmd)) {
        stop();
        return ESP_ERR_INVALID_RESPONSE;
    }
    
    stop();
    return ESP_OK;
}

esp_err_t SoftwareI2C::write_data(uint8_t addr, const uint8_t* data, size_t len) {
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    
    // For SSD1306, send data in chunks to avoid I2C timing issues
    const size_t max_chunk = 32;  // Send 32 bytes at a time
    
    for (size_t offset = 0; offset < len; offset += max_chunk) {
        size_t chunk_size = (len - offset > max_chunk) ? max_chunk : (len - offset);
        
        start();
        
        // Send address with write bit
        if (!write_byte((addr << 1) | 0)) {
            stop();
            ESP_LOGE(TAG, "No ACK for address 0x%02X", addr);
            return ESP_ERR_NOT_FOUND;
        }
        
        // Send control byte (Co=0, D/C#=1 for data)
        if (!write_byte(0x40)) {
            stop();
            ESP_LOGE(TAG, "Failed to send control byte");
            return ESP_ERR_INVALID_RESPONSE;
        }
        
        // Send data bytes for this chunk
        for (size_t i = 0; i < chunk_size; i++) {
            if (!write_byte(data[offset + i])) {
                stop();
                ESP_LOGE(TAG, "NACK at data byte %zu", offset + i);
                return ESP_ERR_INVALID_RESPONSE;
            }
        }
        
        stop();
        
        // Small delay between chunks to let display process
        if (offset + chunk_size < len) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    
    return ESP_OK;
}

esp_err_t SoftwareI2C::ssd1306_command(uint8_t cmd) {
    return write_command(SSD1306_ADDR, cmd);
}

esp_err_t SoftwareI2C::ssd1306_init() {
    ESP_LOGI(TAG, "Initializing SSD1306 over software I2C");
    
    // Wait for display to power up
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Initialization sequence for SSD1306
    const uint8_t init_cmds[] = {
        SSD1306_CMD_DISPLAY_OFF,         // Display off
        SSD1306_CMD_SET_DISPLAY_CLK_DIV, 0x80, // Set clock divide ratio
        SSD1306_CMD_SET_MULTIPLEX, 0x1F, // Set multiplex ratio (1/32 duty for 128x32)
        SSD1306_CMD_SET_DISPLAY_OFFSET, 0x00, // Set display offset
        SSD1306_CMD_SET_START_LINE | 0x00, // Set start line
        SSD1306_CMD_CHARGE_PUMP, 0x14,   // Enable charge pump
        SSD1306_CMD_MEM_ADDR_MODE, 0x00, // Horizontal addressing mode
        SSD1306_CMD_SEG_REMAP | 0x01,    // Segment remap
        SSD1306_CMD_COM_SCAN_DIR | 0x08, // COM scan direction
        SSD1306_CMD_SET_COM_PINS, 0x02,  // COM pins configuration for 128x32
        SSD1306_CMD_SET_CONTRAST, 0x8F,  // Set contrast
        SSD1306_CMD_SET_PRECHARGE, 0xF1, // Set pre-charge period
        SSD1306_CMD_SET_VCOM_DESEL, 0x40, // Set VCOMH deselect level
        SSD1306_CMD_DISPLAY_ALL_ON | 0x00, // Display all on resume
        SSD1306_CMD_NORMAL_DISPLAY,      // Normal display
        SSD1306_CMD_DISPLAY_ON           // Display on
    };
    
    // Send initialization commands
    for (size_t i = 0; i < sizeof(init_cmds); i++) {
        esp_err_t ret = ssd1306_command(init_cmds[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send init command[%d]=0x%02X: %s", 
                     i, init_cmds[i], esp_err_to_name(ret));
            return ret;
        }
        vTaskDelay(pdMS_TO_TICKS(1));  // Small delay between commands
    }
    
    ESP_LOGI(TAG, "SSD1306 initialization complete");
    
    // Clear the display to start fresh
    ssd1306_clear();
    ESP_LOGI(TAG, "Display cleared and ready");
    
    return ESP_OK;
}

esp_err_t SoftwareI2C::ssd1306_clear() {
    ESP_LOGI(TAG, "Clearing SSD1306 display");
    
    // Set column address range (0-127)
    ssd1306_command(SSD1306_CMD_COLUMN_ADDR);
    ssd1306_command(0);    // Start column
    ssd1306_command(127);  // End column
    
    // Set page address range (0-3 for 128x32)
    ssd1306_command(SSD1306_CMD_PAGE_ADDR);
    ssd1306_command(0);    // Start page
    ssd1306_command(3);    // End page (3 for 32-pixel height, 7 for 64-pixel)
    
    // Clear screen data (128 * 4 pages = 512 bytes for 128x32)
    uint8_t clear_data[128] = {0};
    for (int page = 0; page < 4; page++) {
        esp_err_t ret = write_data(SSD1306_ADDR, clear_data, sizeof(clear_data));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to clear page %d", page);
            return ret;
        }
    }
    
    return ESP_OK;
}

esp_err_t SoftwareI2C::ssd1306_display_on(bool on) {
    return ssd1306_command(on ? SSD1306_CMD_DISPLAY_ON : SSD1306_CMD_DISPLAY_OFF);
}

bool SoftwareI2C::probe_address(uint8_t addr) {
    start();
    bool ack = write_byte((addr << 1) | 0);  // Write mode
    stop();
    return ack;
}

void SoftwareI2C::scan_i2c() {
    ESP_LOGI(TAG, "Scanning software I2C bus on SDA=%d, SCL=%d...", sda_pin_, scl_pin_);
    
    int found = 0;
    for (uint8_t addr = 1; addr < 127; addr++) {
        // Skip reserved addresses
        if (addr < 0x08 || addr > 0x77) continue;
        
        if (probe_address(addr)) {
            ESP_LOGI(TAG, "  Found device at address 0x%02X", addr);
            found++;
        }
        
        // Small delay between probes
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    
    if (found == 0) {
        ESP_LOGW(TAG, "  No I2C devices found!");
        ESP_LOGW(TAG, "  Check wiring: SDA to GPIO %d, SCL to GPIO %d", sda_pin_, scl_pin_);
    } else {
        ESP_LOGI(TAG, "  Found %d device(s)", found);
    }
}