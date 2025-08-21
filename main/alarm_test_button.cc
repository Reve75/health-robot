#include "alarm_test_button.h"
#include "application.h"
#include <esp_log.h>
#include <time.h>

const char* AlarmTestButton::TAG = "AlarmTestButton";

AlarmTestButton::AlarmTestButton(gpio_num_t gpio_num) : gpio_num_(gpio_num) {
    ESP_LOGI(TAG, "Initializing alarm test button on GPIO %d", gpio_num);
    
    // Create button with internal pull-up, active low (button connects to ground)
    // Long press time: 2000ms, short press time: 50ms
    button_ = new Button(gpio_num, false, 2000, 50);
    
    // Set up default behaviors
    button_->OnClick([this]() {
        ESP_LOGI(TAG, "Short press detected - setting test alarm");
        if (on_set_alarm_) {
            on_set_alarm_();
        } else {
            // Default behavior: Set alarm for 1 minute from now
            auto& app = Application::GetInstance();
            time_t now = time(nullptr);
            struct tm *timeinfo = localtime(&now);
            
            if (timeinfo != nullptr) {
                uint8_t test_hour = timeinfo->tm_hour;
                uint8_t test_minute = (timeinfo->tm_min + 1) % 60;
                if (timeinfo->tm_min == 59) {
                    test_hour = (test_hour + 1) % 24;
                }
                
                app.SetAlarm(test_hour, test_minute);
                ESP_LOGI(TAG, "Test alarm set for %02d:%02d", test_hour, test_minute);
            }
        }
    });
    
    button_->OnLongPress([this]() {
        ESP_LOGI(TAG, "=== BUTTON LONG PRESS DETECTED (2 seconds) ===");
        ESP_LOGI(TAG, "GPIO %d long press callback triggered", gpio_num_);
        if (on_clear_alarms_) {
            ESP_LOGI(TAG, "Custom long press handler set, calling it");
            on_clear_alarms_();
        } else {
            ESP_LOGI(TAG, "Using default long press handler - starting BLE config mode");
            // Default behavior: Start BLE configuration mode
            auto& app = Application::GetInstance();
            ESP_LOGI(TAG, "Got Application instance, calling StartBleConfigMode");
            app.StartBleConfigMode();
            ESP_LOGI(TAG, "StartBleConfigMode returned");
        }
    });
    
    button_->OnDoubleClick([this]() {
        ESP_LOGI(TAG, "Double click detected - stopping alarm");
        if (on_stop_alarm_) {
            on_stop_alarm_();
        } else {
            // Default behavior: Stop alarm if ringing
            auto& app = Application::GetInstance();
            app.StopAlarm();
        }
    });
}

AlarmTestButton::~AlarmTestButton() {
    delete button_;
}

void AlarmTestButton::OnSetAlarmPress(std::function<void()> callback) {
    on_set_alarm_ = callback;
}

void AlarmTestButton::OnClearAlarmsPress(std::function<void()> callback) {
    on_clear_alarms_ = callback;
}

void AlarmTestButton::OnStopAlarmPress(std::function<void()> callback) {
    on_stop_alarm_ = callback;
}