#ifndef ESP32_CAMERA_H
#define ESP32_CAMERA_H

// Camera support disabled due to I2C driver conflict with esp32-camera component
// The esp32-camera component uses old I2C driver API which conflicts with new API
#ifndef CONFIG_CAMERA_DISABLED
#define CONFIG_CAMERA_DISABLED 1
#endif

#ifdef CONFIG_CAMERA_DISABLED

// Stub implementation when camera is disabled
#include <lvgl.h>
#include <thread>
#include <memory>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "camera.h"

// Stub for camera_config_t when esp_camera.h is not available
struct camera_config_t {
    int dummy;
};

// Stub for camera_fb_t
struct camera_fb_t {
    uint8_t* buf;
    size_t len;
};

struct JpegChunk {
    uint8_t* data;
    size_t len;
};

class Esp32Camera : public Camera {
private:
    camera_fb_t* fb_ = nullptr;
    lv_img_dsc_t preview_image_;
    std::string explain_url_;
    std::string explain_token_;
    std::thread encoder_thread_;

public:
    Esp32Camera(const camera_config_t& config) {}
    ~Esp32Camera() {}

    virtual void SetExplainUrl(const std::string& url, const std::string& token) {}
    virtual bool Capture() { return false; }
    virtual bool SetHMirror(bool enabled) override { return false; }
    virtual bool SetVFlip(bool enabled) override { return false; }
    virtual std::string Explain(const std::string& question) { return ""; }
};

#else
// Original implementation with camera support
#include <esp_camera.h>
#include <lvgl.h>
#include <thread>
#include <memory>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "camera.h"

struct JpegChunk {
    uint8_t* data;
    size_t len;
};

class Esp32Camera : public Camera {
private:
    camera_fb_t* fb_ = nullptr;
    lv_img_dsc_t preview_image_;
    std::string explain_url_;
    std::string explain_token_;
    std::thread encoder_thread_;

public:
    Esp32Camera(const camera_config_t& config);
    ~Esp32Camera();

    virtual void SetExplainUrl(const std::string& url, const std::string& token);
    virtual bool Capture();
    // 翻转控制函数
    virtual bool SetHMirror(bool enabled) override;
    virtual bool SetVFlip(bool enabled) override;
    virtual std::string Explain(const std::string& question);
};

#endif // CONFIG_CAMERA_DISABLED

#endif // ESP32_CAMERA_H