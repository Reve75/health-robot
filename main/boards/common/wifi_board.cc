#include "wifi_board.h"

#include "display.h"
#include "application.h"
#include "system_info.h"
#include "font_awesome_symbols.h"
#include "settings.h"
#include "assets/lang_config.h"
#include "ble_config.h"  // Add BLE configuration support
#include "config/server_config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_http.h>
#include <esp_mqtt.h>
#include <esp_udp.h>
#include <tcp_transport.h>
#include <tls_transport.h>
#include <web_socket.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_wifi.h>

#include <wifi_station.h>
#include <wifi_configuration_ap.h>
#include <ssid_manager.h>
#include "afsk_demod.h"

static const char *TAG = "WifiBoard";

WifiBoard::WifiBoard() {
    Settings settings("wifi", true);
    wifi_config_mode_ = settings.GetInt("force_ap") == 1;
    if (wifi_config_mode_) {
        ESP_LOGI(TAG, "force_ap is set to 1, reset to 0");
        settings.SetInt("force_ap", 0);
    }
}

std::string WifiBoard::GetBoardType() {
    return "wifi";
}

void WifiBoard::EnterBleConfigMode() {
    ESP_LOGI(TAG, "=== EnterBleConfigMode CALLED ===");
    auto& application = Application::GetInstance();
    ESP_LOGI(TAG, "Setting device state to kDeviceStateWifiConfiguring");
    application.SetDeviceState(kDeviceStateWifiConfiguring);
    
    // No need to stop WiFi - we'll just save credentials and reboot
    ESP_LOGI(TAG, "Getting device information for BLE");
    
    // Get device information
    std::string uuid = Board::GetInstance().GetUuid();
    std::string mac = SystemInfo::GetMacAddress();
    ESP_LOGI(TAG, "Device UUID: %s, MAC: %s", uuid.c_str(), mac.c_str());
    
    // Generate BLE device name with last 4 hex digits
    uint8_t mac_bytes[6];
    esp_read_mac(mac_bytes, ESP_MAC_WIFI_STA);
    char device_name[32];
    snprintf(device_name, sizeof(device_name), "Xiaozhi-%02X%02X", mac_bytes[4], mac_bytes[5]);
    
    ESP_LOGI(TAG, "BLE device name will be: %s", device_name);
    
    // Setup BLE configuration service
    auto& ble_config = BleConfig::GetInstance();
    ble_config.SetDeviceInfo(uuid, mac);
    
    // Set callback for when WiFi credentials are received
    ble_config.SetConfigCallback([this](const BleConfig::WifiCredentials& creds) {
        ESP_LOGI(TAG, "WiFi credentials received via BLE");
        ESP_LOGI(TAG, "User ID: %s", creds.userId.c_str());
        ESP_LOGI(TAG, "SSID: %s", creds.ssid.c_str());
        
        // Notify connecting status
        auto& ble_config = BleConfig::GetInstance();
        ble_config.NotifyStatus(BleConfig::STATUS_CONNECTING, "Connecting to WiFi...");
        
        // Try to connect to WiFi
        auto& wifi_station = WifiStation::GetInstance();
        
        // Add WiFi credentials
        auto& ssid_manager = SsidManager::GetInstance();
        ssid_manager.AddSsid(creds.ssid, creds.password);
        
        // Start WiFi connection
        wifi_station.Start();
        
        // Wait for connection (30 seconds timeout)
        if (wifi_station.WaitForConnected(30000)) {
            ESP_LOGI(TAG, "WiFi connected successfully");
            
            // Store user ID in NVS
            Settings device_settings("device", true);
            device_settings.SetString("user_id", creds.userId);
            ESP_LOGI(TAG, "User ID stored: %s", creds.userId.c_str());
            
            // Update server URLs to the new server
            {
                Settings ws_settings("websocket", true);
                std::string old_url = ws_settings.GetString("url");
                ESP_LOGI(TAG, "Before update - WebSocket URL: %s", old_url.c_str());
                
                ws_settings.SetString("url", ServerConfig::GetInstance().GetWebSocketUrl());
                ws_settings.SetInt("version", 1);
                
                std::string new_url = ws_settings.GetString("url");
                ESP_LOGI(TAG, "After update - WebSocket URL: %s", new_url.c_str());
            }
            
            {
                Settings wifi_settings("wifi", true);
                wifi_settings.SetString("ota_url", ServerConfig::GetInstance().GetOtaUrl());
                ESP_LOGI(TAG, "OTA URL updated to: %s", ServerConfig::GetInstance().GetOtaUrl().c_str());
            }
            
            // Notify success
            ble_config.NotifyStatus(BleConfig::STATUS_CONNECTED, "Connected! Rebooting...");
            
            // Wait a bit for notification to be sent
            vTaskDelay(pdMS_TO_TICKS(2000));
            
            // Stop BLE
            ble_config.Stop();
            
            // Reboot to apply configuration
            ESP_LOGI(TAG, "Rebooting to apply configuration...");
            esp_restart();
        } else {
            ESP_LOGE(TAG, "Failed to connect to WiFi");
            ble_config.NotifyStatus(BleConfig::STATUS_FAILED, "WiFi connection failed");
            
            // Stop WiFi and stay in BLE mode for retry
            wifi_station.Stop();
        }
    });
    
    // Start BLE service
    ESP_LOGI(TAG, "Starting BLE service with device name: %s", device_name);
    ble_config.Start(device_name);
    ESP_LOGI(TAG, "BLE service started successfully");
    
    // Display BLE configuration info
    std::string hint = "BLE Configuration Mode\n";
    hint += "Device: " + std::string(device_name) + "\n";
    hint += "Open Xiaozhi app to configure";
    
    auto display = GetDisplay();
    if (display) {
        ESP_LOGI(TAG, "Showing BLE notification on display");
        display->ShowNotification(hint.c_str());
    }
    
    ESP_LOGI(TAG, "BLE configuration mode is now active");
    // Don't block here - let the BLE callbacks handle everything
    // The device will reboot after successful configuration
}

void WifiBoard::EnterWifiConfigMode() {
    // Keep old WiFi AP mode as fallback
    auto& application = Application::GetInstance();
    application.SetDeviceState(kDeviceStateWifiConfiguring);

    auto& wifi_ap = WifiConfigurationAp::GetInstance();
    wifi_ap.SetLanguage(Lang::CODE);
    
    // Generate unique SSID with last 4 hex digits of MAC
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "Xiaozhi-%02X%02X", mac[4], mac[5]);
    wifi_ap.SetSsidPrefix(ssid);
    wifi_ap.Start();

    // 显示 WiFi 配置 AP 的 SSID 和 Web 服务器 URL
    std::string hint = Lang::Strings::CONNECT_TO_HOTSPOT;
    hint += wifi_ap.GetSsid();
    hint += Lang::Strings::ACCESS_VIA_BROWSER;
    hint += wifi_ap.GetWebServerUrl();
    hint += "\n\n";
    
    // 播报配置 WiFi 的提示
    application.Alert(Lang::Strings::WIFI_CONFIG_MODE, hint.c_str(), "", Lang::Sounds::P3_WIFICONFIG);

    #if USE_ACOUSTIC_WIFI_PROVISIONING
    audio_wifi_config::ReceiveWifiCredentialsFromAudio(&application, &wifi_ap);
    #endif
    
    // Wait forever until reset after configuration
    while (true) {
        int free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        int min_free_sram = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        ESP_LOGI(TAG, "Free internal: %u minimal internal: %u", free_sram, min_free_sram);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void WifiBoard::StartNetwork() {
    // User can press BOOT button while starting to force WiFi AP configuration mode
    // This provides a fallback if BLE configuration doesn't work
    if (wifi_config_mode_) {
        EnterWifiConfigMode();  // Force WiFi AP mode if button pressed
        return;
    }

    // Check if both user_id and WiFi are configured
    Settings device_settings("device", true);
    std::string user_id = device_settings.GetString("user_id");
    
    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();
    
    if (user_id.empty() || ssid_list.empty()) {
        // Missing configuration - enter BLE configuration mode
        if (user_id.empty()) {
            ESP_LOGI(TAG, "No user ID configured");
        }
        if (ssid_list.empty()) {
            ESP_LOGI(TAG, "No WiFi configured");
        }
        
        // Set default server URLs (can be overridden during setup)
        Settings ws_settings("websocket", true);
        std::string existing_url = ws_settings.GetString("url");
        ESP_LOGI(TAG, "Existing WebSocket URL in NVS: '%s' (length: %zu)", existing_url.c_str(), existing_url.length());
        if (existing_url.empty()) {
            ESP_LOGI(TAG, "WebSocket URL is empty, setting default from configuration");
            ws_settings.SetString("url", ServerConfig::GetInstance().GetWebSocketUrl());
            ws_settings.SetInt("version", 1);
        } else {
            ESP_LOGI(TAG, "WebSocket URL already exists, not overwriting: %s", existing_url.c_str());
        }
        
        Settings wifi_settings("wifi", true);
        if (wifi_settings.GetString("ota_url").empty()) {
            wifi_settings.SetString("ota_url", ServerConfig::GetInstance().GetOtaUrl());
        }
        
        ESP_LOGI(TAG, "Entering BLE configuration mode");
        EnterBleConfigMode();  // Use BLE mode instead of WiFi AP
        
        // Wait forever until configured or reset
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(10000));
            ESP_LOGI(TAG, "Waiting for BLE configuration...");
        }
    }
    
    ESP_LOGI(TAG, "Configuration found - User ID: %s", user_id.c_str());
    
    // Debug: Check what WebSocket URL is stored after reboot
    {
        Settings ws_debug("websocket", false);  // Read-only
        std::string stored_url = ws_debug.GetString("url");
        ESP_LOGI(TAG, "After reboot - WebSocket URL from NVS: %s", stored_url.c_str());
        
        // If old IP detected, force update to new IP
        if (stored_url.find("192.168.8.157") != std::string::npos) {
            ESP_LOGW(TAG, "Old server IP detected! Force updating to new IP...");
            Settings ws_fix("websocket", true);
            ws_fix.SetString("url", ServerConfig::GetInstance().GetWebSocketUrl());
            ws_fix.SetInt("version", 1);
            ESP_LOGI(TAG, "WebSocket URL force updated to: %s", ServerConfig::GetInstance().GetWebSocketUrl().c_str());
        }
    }

    auto& wifi_station = WifiStation::GetInstance();
    wifi_station.OnScanBegin([this]() {
        auto display = Board::GetInstance().GetDisplay();
        display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
    });
    wifi_station.OnConnect([this](const std::string& ssid) {
        auto display = Board::GetInstance().GetDisplay();
        std::string notification = Lang::Strings::CONNECT_TO;
        notification += ssid;
        notification += "...";
        display->ShowNotification(notification.c_str(), 30000);
    });
    wifi_station.OnConnected([this](const std::string& ssid) {
        auto display = Board::GetInstance().GetDisplay();
        std::string notification = Lang::Strings::CONNECTED_TO;
        notification += ssid;
        display->ShowNotification(notification.c_str(), 30000);
    });
    wifi_station.Start();

    // Try to connect to WiFi, if failed, launch BLE configuration mode
    if (!wifi_station.WaitForConnected(60 * 1000)) {
        wifi_station.Stop();
        ESP_LOGI(TAG, "WiFi connection failed, entering BLE configuration mode");
        EnterBleConfigMode();
        
        // Wait forever until configured or reset
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(10000));
            ESP_LOGI(TAG, "Waiting for BLE configuration...");
        }
    }
}

Http* WifiBoard::CreateHttp() {
    return new EspHttp();
}

WebSocket* WifiBoard::CreateWebSocket() {
    Settings settings("websocket", false);
    std::string url = settings.GetString("url");
    if (url.find("wss://") == 0) {
        return new WebSocket(new TlsTransport());
    } else {
        return new WebSocket(new TcpTransport());
    }
    return nullptr;
}

Mqtt* WifiBoard::CreateMqtt() {
    return new EspMqtt();
}

Udp* WifiBoard::CreateUdp() {
    return new EspUdp();
}

const char* WifiBoard::GetNetworkStateIcon() {
    if (wifi_config_mode_) {
        return FONT_AWESOME_WIFI;
    }
    auto& wifi_station = WifiStation::GetInstance();
    if (!wifi_station.IsConnected()) {
        return FONT_AWESOME_WIFI_OFF;
    }
    int8_t rssi = wifi_station.GetRssi();
    if (rssi >= -60) {
        return FONT_AWESOME_WIFI;
    } else if (rssi >= -70) {
        return FONT_AWESOME_WIFI_FAIR;
    } else {
        return FONT_AWESOME_WIFI_WEAK;
    }
}

std::string WifiBoard::GetBoardJson() {
    // Set the board type for OTA
    auto& wifi_station = WifiStation::GetInstance();
    std::string board_json = R"({)";
    board_json += R"("type":")" + std::string(BOARD_TYPE) + R"(",)";
    board_json += R"("name":")" + std::string(BOARD_NAME) + R"(",)";
    // Only include WiFi info if not in AP mode (wifi_config_mode_ is for AP mode)
    if (!wifi_config_mode_) {
        board_json += R"("ssid":")" + wifi_station.GetSsid() + R"(",)";
        board_json += R"("rssi":)" + std::to_string(wifi_station.GetRssi()) + R"(,)";
        board_json += R"("channel":)" + std::to_string(wifi_station.GetChannel()) + R"(,)";
        board_json += R"("ip":")" + wifi_station.GetIpAddress() + R"(",)";
    }
    board_json += R"("mac":")" + SystemInfo::GetMacAddress() + R"(")";
    board_json += R"(})";
    return board_json;
}

void WifiBoard::SetPowerSaveMode(bool enabled) {
    auto& wifi_station = WifiStation::GetInstance();
    wifi_station.SetPowerSaveMode(enabled);
}

void WifiBoard::ResetWifiConfiguration() {
    // Set a flag and reboot the device to enter the network configuration mode
    {
        Settings settings("wifi", true);
        settings.SetInt("force_ap", 1);
    }
    GetDisplay()->ShowNotification(Lang::Strings::ENTERING_WIFI_CONFIG_MODE);
    vTaskDelay(pdMS_TO_TICKS(1000));
    // Reboot the device
    esp_restart();
}

std::string WifiBoard::GetDeviceStatusJson() {
    /*
     * 返回设备状态JSON
     * 
     * 返回的JSON结构如下：
     * {
     *     "audio_speaker": {
     *         "volume": 70
     *     },
     *     "screen": {
     *         "brightness": 100,
     *         "theme": "light"
     *     },
     *     "battery": {
     *         "level": 50,
     *         "charging": true
     *     },
     *     "network": {
     *         "type": "wifi",
     *         "ssid": "Xiaozhi",
     *         "rssi": -60
     *     },
     *     "chip": {
     *         "temperature": 25
     *     }
     * }
     */
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    // Audio speaker
    auto audio_speaker = cJSON_CreateObject();
    auto audio_codec = board.GetAudioCodec();
    if (audio_codec) {
        cJSON_AddNumberToObject(audio_speaker, "volume", audio_codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    // Screen brightness
    auto backlight = board.GetBacklight();
    auto screen = cJSON_CreateObject();
    if (backlight) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    auto display = board.GetDisplay();
    if (display && display->height() > 64) { // For LCD display only
        cJSON_AddStringToObject(screen, "theme", display->GetTheme().c_str());
    }
    cJSON_AddItemToObject(root, "screen", screen);

    // Battery
    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        cJSON* battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", battery_level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(root, "battery", battery);
    }

    // Network
    auto network = cJSON_CreateObject();
    auto& wifi_station = WifiStation::GetInstance();
    cJSON_AddStringToObject(network, "type", "wifi");
    cJSON_AddStringToObject(network, "ssid", wifi_station.GetSsid().c_str());
    int rssi = wifi_station.GetRssi();
    if (rssi >= -60) {
        cJSON_AddStringToObject(network, "signal", "strong");
    } else if (rssi >= -70) {
        cJSON_AddStringToObject(network, "signal", "medium");
    } else {
        cJSON_AddStringToObject(network, "signal", "weak");
    }
    cJSON_AddItemToObject(root, "network", network);

    // Chip
    float esp32temp = 0.0f;
    if (board.GetTemperature(esp32temp)) {
        auto chip = cJSON_CreateObject();
        cJSON_AddNumberToObject(chip, "temperature", esp32temp);
        cJSON_AddItemToObject(root, "chip", chip);
    }

    auto json_str = cJSON_PrintUnformatted(root);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return json;
}
