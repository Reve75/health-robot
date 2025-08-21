/*
 * BLE Configuration Service for ESP32
 * Handles WiFi configuration via Bluetooth Low Energy
 * This replaces the WiFi AP/WebView approach to avoid iOS captive portal issues
 */

#ifndef BLE_CONFIG_H
#define BLE_CONFIG_H

#include <string>
#include <functional>

// BLE Service and Characteristic UUIDs
#define BLE_CONFIG_SERVICE_UUID        "12345678-1234-5678-1234-56789abcdef0"
#define BLE_CHAR_DEVICE_INFO_UUID      "12345678-1234-5678-1234-56789abcdef1"  // Read
#define BLE_CHAR_WIFI_CONFIG_UUID      "12345678-1234-5678-1234-56789abcdef2"  // Write
#define BLE_CHAR_STATUS_UUID            "12345678-1234-5678-1234-56789abcdef3"  // Notify

class BleConfig {
public:
    enum ConfigStatus {
        STATUS_WAITING,
        STATUS_CONNECTING,
        STATUS_CONNECTED,
        STATUS_FAILED
    };

    struct WifiCredentials {
        std::string userId;
        std::string ssid;
        std::string password;
    };

    using ConfigCallback = std::function<void(const WifiCredentials&)>;
    
    static BleConfig& GetInstance();
    
    // Start BLE advertising and GATT server
    void Start(const std::string& deviceName);
    
    // Stop BLE service
    void Stop();
    
    // Start advertising (used internally by NimBLE implementation)
    void StartAdvertising();
    
    // Set device information to advertise
    void SetDeviceInfo(const std::string& uuid, const std::string& mac);
    
    // Set callback for when WiFi credentials are received
    void SetConfigCallback(ConfigCallback callback);
    
    // Notify status to connected client
    void NotifyStatus(ConfigStatus status, const std::string& message);
    
    // Check if BLE is currently active
    bool IsActive() const { return active_; }

private:
    BleConfig();
    ~BleConfig();
    
    bool active_ = false;
    std::string device_uuid_;
    std::string device_mac_;
    
    // BLE handles (implementation specific)
    void* server_handle_ = nullptr;
    void* service_handle_ = nullptr;
    void* char_device_info_ = nullptr;
    void* char_wifi_config_ = nullptr;
    void* char_status_ = nullptr;
    
    // Internal handlers
    void OnWifiConfigWrite(const std::string& value);
    std::string OnDeviceInfoRead();
    
public:
    // Make this public for static callback access
    ConfigCallback config_callback_;
};

#endif // BLE_CONFIG_H