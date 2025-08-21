#ifndef ALARM_TEST_BUTTON_H_
#define ALARM_TEST_BUTTON_H_

#include "boards/common/button.h"
#include <functional>
#include <esp_log.h>

class AlarmTestButton {
public:
    // Constructor with configurable GPIO pin (default GPIO_NUM_48 or any free pin)
    AlarmTestButton(gpio_num_t gpio_num = GPIO_NUM_48);
    ~AlarmTestButton();

    // Set callback for short press (set alarm)
    void OnSetAlarmPress(std::function<void()> callback);
    
    // Set callback for long press (clear all alarms)
    void OnClearAlarmsPress(std::function<void()> callback);
    
    // Set callback for double click (stop ringing alarm)
    void OnStopAlarmPress(std::function<void()> callback);

private:
    Button* button_;
    gpio_num_t gpio_num_;
    
    std::function<void()> on_set_alarm_;
    std::function<void()> on_clear_alarms_;
    std::function<void()> on_stop_alarm_;
    
    static const char* TAG;
};

#endif // ALARM_TEST_BUTTON_H_