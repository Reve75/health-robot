#pragma once

#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class GPIOTest {
private:
    static constexpr const char* TAG = "GPIOTest";
    
public:
    // Test GPIO pins by toggling them and checking they work
    static void test_pins() {
        ESP_LOGI(TAG, "=== GPIO PIN TEST ===");
        ESP_LOGI(TAG, "Testing GPIO 11 and GPIO 12...");
        
        // Configure as outputs first
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = (1ULL << 11) | (1ULL << 12);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio_config(&io_conf);
        
        // Toggle pins 5 times
        for (int i = 0; i < 5; i++) {
            ESP_LOGI(TAG, "Setting GPIO 11=HIGH, GPIO 12=LOW");
            gpio_set_level(GPIO_NUM_11, 1);
            gpio_set_level(GPIO_NUM_12, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
            
            ESP_LOGI(TAG, "Setting GPIO 11=LOW, GPIO 12=HIGH");
            gpio_set_level(GPIO_NUM_11, 0);
            gpio_set_level(GPIO_NUM_12, 1);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        
        // Now test as inputs with pull-ups
        ESP_LOGI(TAG, "Testing as inputs with pull-ups...");
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&io_conf);
        
        // Read values
        int val11 = gpio_get_level(GPIO_NUM_11);
        int val12 = gpio_get_level(GPIO_NUM_12);
        ESP_LOGI(TAG, "GPIO 11 reads: %d (should be 1 with pull-up)", val11);
        ESP_LOGI(TAG, "GPIO 12 reads: %d (should be 1 with pull-up)", val12);
        
        ESP_LOGI(TAG, "=== GPIO TEST COMPLETE ===");
        ESP_LOGI(TAG, "If you have a multimeter:");
        ESP_LOGI(TAG, "- GPIO 11 should measure 3.3V with pull-up");
        ESP_LOGI(TAG, "- GPIO 12 should measure 3.3V with pull-up");
    }
};