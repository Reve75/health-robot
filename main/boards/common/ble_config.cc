#include "ble_config.h"

#include <esp_log.h>
#include <cJSON.h>
#include <cstring>

// BLE includes - these require CONFIG_BT_ENABLED
#ifdef CONFIG_BT_ENABLED
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>
#include <esp_gatts_api.h>
#include <esp_gatt_common_api.h>
#include <esp_bt_device.h>
#endif

static const char* TAG = "BleConfig";

#ifdef CONFIG_BT_ENABLED

// Simple BLE implementation
#define PROFILE_NUM 1
#define PROFILE_APP_ID 0
#define ESP_APP_ID 0x55
#define SVC_INST_ID 0

// Simplified characteristic value attributes
#define GATTS_CHAR_VAL_LEN_MAX 512

static uint8_t char_str[] = {0x11, 0x22, 0x33};
static esp_gatt_char_prop_t a_property = 0;
static esp_attr_value_t gatts_attr_val = {
    .attr_max_len = GATTS_CHAR_VAL_LEN_MAX,
    .attr_len = sizeof(char_str),
    .attr_value = char_str,
};

struct gatts_profile_inst {
    esp_gatts_cb_t gatts_cb;
    uint16_t gatts_if;
    uint16_t app_id;
    uint16_t conn_id;
    uint16_t service_handle;
    esp_gatt_srvc_id_t service_id;
    uint16_t char_handle;
    esp_bt_uuid_t char_uuid;
    esp_gatt_perm_t perm;
    esp_gatt_char_prop_t property;
    uint16_t descr_handle;
    esp_bt_uuid_t descr_uuid;
};

// Forward declaration
static void gatts_profile_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);

static struct gatts_profile_inst gl_profile_tab[PROFILE_NUM] = {
    [PROFILE_APP_ID] = {
        .gatts_cb = gatts_profile_event_handler,
        .gatts_if = ESP_GATT_IF_NONE,
    },
};

// Static instance pointer for callbacks
static BleConfig* s_instance = nullptr;

// Device info storage
static std::string device_info_json;
static uint16_t device_info_handle = 0;
static uint16_t wifi_config_handle = 0;
static uint16_t status_handle = 0;
static uint16_t current_conn_id = 0;

// GATTS event handler
static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, 
                                esp_ble_gatts_cb_param_t *param) {
    // If the gatts_if is not for any of our application profiles, ignore
    if (event == ESP_GATTS_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) {
            gl_profile_tab[param->reg.app_id].gatts_if = gatts_if;
        } else {
            ESP_LOGI(TAG, "reg app failed, app_id %04x, status %d",
                    param->reg.app_id, param->reg.status);
            return;
        }
    }
    
    // Call the profile event handler
    for (int idx = 0; idx < PROFILE_NUM; idx++) {
        if (gatts_if == ESP_GATT_IF_NONE || 
                gatts_if == gl_profile_tab[idx].gatts_if) {
            if (gl_profile_tab[idx].gatts_cb) {
                gl_profile_tab[idx].gatts_cb(event, gatts_if, param);
            }
        }
    }
}

// Profile event handler - simplified
static void gatts_profile_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, 
                                        esp_ble_gatts_cb_param_t *param) {
    switch (event) {
        case ESP_GATTS_REG_EVT: {
            ESP_LOGI(TAG, "REGISTER_APP_EVT, status %d, app_id %d", param->reg.status, param->reg.app_id);
            
            // Create a simple service
            gl_profile_tab[PROFILE_APP_ID].service_id.is_primary = true;
            gl_profile_tab[PROFILE_APP_ID].service_id.id.inst_id = SVC_INST_ID;
            gl_profile_tab[PROFILE_APP_ID].service_id.id.uuid.len = ESP_UUID_LEN_16;
            gl_profile_tab[PROFILE_APP_ID].service_id.id.uuid.uuid.uuid16 = 0x00FF; // Simple test service
            
            esp_ble_gatts_create_service(gatts_if, &gl_profile_tab[PROFILE_APP_ID].service_id, 10);
            break;
        }
        
        case ESP_GATTS_CREATE_EVT: {
            ESP_LOGI(TAG, "CREATE_SERVICE_EVT, status %d, service_handle %d", 
                     param->create.status, param->create.service_handle);
            
            gl_profile_tab[PROFILE_APP_ID].service_handle = param->create.service_handle;
            
            // Start service
            esp_ble_gatts_start_service(gl_profile_tab[PROFILE_APP_ID].service_handle);
            
            // Add a simple characteristic
            a_property = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
            esp_bt_uuid_t char_uuid = {0};
            char_uuid.len = ESP_UUID_LEN_16;
            char_uuid.uuid.uuid16 = 0xFF01; // Simple characteristic UUID
            
            esp_ble_gatts_add_char(gl_profile_tab[PROFILE_APP_ID].service_handle,
                                  &char_uuid,
                                  ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                  a_property,
                                  &gatts_attr_val,
                                  NULL);
            break;
        }
        
        case ESP_GATTS_ADD_CHAR_EVT: {
            ESP_LOGI(TAG, "ADD_CHAR_EVT, status %d, attr_handle %d", 
                     param->add_char.status, param->add_char.attr_handle);
            
            // Store the characteristic handle for later use
            if (device_info_handle == 0) {
                device_info_handle = param->add_char.attr_handle;
                ESP_LOGI(TAG, "Device info handle: %d", device_info_handle);
                
                // Add second characteristic for WiFi config
                esp_bt_uuid_t char_uuid = {0};
                char_uuid.len = ESP_UUID_LEN_16;
                char_uuid.uuid.uuid16 = 0xFF02;
                
                esp_ble_gatts_add_char(gl_profile_tab[PROFILE_APP_ID].service_handle,
                                      &char_uuid,
                                      ESP_GATT_PERM_WRITE,
                                      ESP_GATT_CHAR_PROP_BIT_WRITE,
                                      &gatts_attr_val,
                                      NULL);
            } else if (wifi_config_handle == 0) {
                wifi_config_handle = param->add_char.attr_handle;
                ESP_LOGI(TAG, "WiFi config handle: %d", wifi_config_handle);
                
                // Add third characteristic for status notifications
                esp_bt_uuid_t char_uuid = {0};
                char_uuid.len = ESP_UUID_LEN_16;
                char_uuid.uuid.uuid16 = 0xFF03;
                
                esp_ble_gatts_add_char(gl_profile_tab[PROFILE_APP_ID].service_handle,
                                      &char_uuid,
                                      ESP_GATT_PERM_READ,
                                      ESP_GATT_CHAR_PROP_BIT_NOTIFY,
                                      &gatts_attr_val,
                                      NULL);
            } else if (status_handle == 0) {
                status_handle = param->add_char.attr_handle;
                ESP_LOGI(TAG, "Status handle: %d", status_handle);
            }
            break;
        }
        
        case ESP_GATTS_START_EVT: {
            ESP_LOGI(TAG, "SERVICE_START_EVT, status %d, service_handle %d",
                     param->start.status, param->start.service_handle);
            break;
        }
        
        case ESP_GATTS_CONNECT_EVT: {
            ESP_LOGI(TAG, "Device connected, conn_id %d", param->connect.conn_id);
            current_conn_id = param->connect.conn_id;
            gl_profile_tab[PROFILE_APP_ID].conn_id = param->connect.conn_id;
            break;
        }
        
        case ESP_GATTS_DISCONNECT_EVT: {
            ESP_LOGI(TAG, "Device disconnected");
            current_conn_id = 0;
            gl_profile_tab[PROFILE_APP_ID].conn_id = 0;
            break;
        }
        
        case ESP_GATTS_READ_EVT: {
            ESP_LOGI(TAG, "READ_EVT, handle %d", param->read.handle);
            
            esp_gatt_rsp_t rsp = {0};
            rsp.attr_value.handle = param->read.handle;
            
            if (param->read.handle == device_info_handle && s_instance) {
                // Return device info
                rsp.attr_value.len = device_info_json.length();
                if (rsp.attr_value.len > ESP_GATT_MAX_ATTR_LEN) {
                    rsp.attr_value.len = ESP_GATT_MAX_ATTR_LEN;
                }
                memcpy(rsp.attr_value.value, device_info_json.c_str(), rsp.attr_value.len);
            } else {
                // Default response
                rsp.attr_value.len = 4;
                rsp.attr_value.value[0] = 0x0a;
                rsp.attr_value.value[1] = 0x0b;
                rsp.attr_value.value[2] = 0x0c;
                rsp.attr_value.value[3] = 0x0d;
            }
            
            esp_ble_gatts_send_response(gatts_if, param->read.conn_id, param->read.trans_id,
                                       ESP_GATT_OK, &rsp);
            break;
        }
        
        case ESP_GATTS_WRITE_EVT: {
            ESP_LOGI(TAG, "WRITE_EVT, handle %d, value len %d", 
                     param->write.handle, param->write.len);
            
            if (param->write.handle == wifi_config_handle && s_instance && param->write.len > 0) {
                // Handle WiFi config write
                std::string value((char*)param->write.value, param->write.len);
                ESP_LOGI(TAG, "Received WiFi config: %s", value.c_str());
                
                // Parse JSON
                cJSON* root = cJSON_Parse(value.c_str());
                if (root) {
                    BleConfig::WifiCredentials creds;
                    
                    cJSON* userId = cJSON_GetObjectItem(root, "userId");
                    if (userId && cJSON_IsString(userId)) {
                        creds.userId = userId->valuestring;
                    }
                    
                    cJSON* ssid = cJSON_GetObjectItem(root, "ssid");
                    if (ssid && cJSON_IsString(ssid)) {
                        creds.ssid = ssid->valuestring;
                    }
                    
                    cJSON* password = cJSON_GetObjectItem(root, "password");
                    if (password && cJSON_IsString(password)) {
                        creds.password = password->valuestring;
                    }
                    
                    cJSON_Delete(root);
                    
                    // Call the callback if credentials are valid
                    if (!creds.userId.empty() && !creds.ssid.empty()) {
                        ESP_LOGI(TAG, "Valid credentials received - User: %s, SSID: %s", 
                                 creds.userId.c_str(), creds.ssid.c_str());
                        if (s_instance->config_callback_) {
                            s_instance->config_callback_(creds);
                        }
                    }
                }
            }
            
            // Send write response if needed
            if (param->write.need_rsp) {
                esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id,
                                           ESP_GATT_OK, NULL);
            }
            break;
        }
        
        default:
            break;
    }
}

BleConfig& BleConfig::GetInstance() {
    static BleConfig instance;
    return instance;
}

BleConfig::BleConfig() {
    s_instance = this;
}

BleConfig::~BleConfig() {
    Stop();
    s_instance = nullptr;
}

void BleConfig::Start(const std::string& deviceName) {
    if (active_) {
        ESP_LOGW(TAG, "BLE already active");
        return;
    }
    
    ESP_LOGI(TAG, "Starting BLE configuration service: %s", deviceName.c_str());
    
    // Release memory for Classic BT (we only use BLE)
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    
    // Initialize Bluetooth controller
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(TAG, "Bluetooth controller init failed: %s", esp_err_to_name(ret));
        return;
    }
    
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(TAG, "Bluetooth controller enable failed: %s", esp_err_to_name(ret));
        return;
    }
    
    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(ret));
        return;
    }
    
    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret));
        return;
    }
    
    // Register GAP callback for advertising
    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret) {
        ESP_LOGE(TAG, "GAP register callback failed: %s", esp_err_to_name(ret));
        return;
    }
    
    // Register GATT callback
    ret = esp_ble_gatts_register_callback(gatts_event_handler);
    if (ret) {
        ESP_LOGE(TAG, "GATTS register callback failed: %s", esp_err_to_name(ret));
        return;
    }
    
    ret = esp_ble_gatts_app_register(ESP_APP_ID);
    if (ret) {
        ESP_LOGE(TAG, "GATTS app register failed: %s", esp_err_to_name(ret));
        return;
    }
    
    // Set device name
    esp_ble_gap_set_device_name(deviceName.c_str());
    
    // Configure advertising data
    esp_ble_adv_data_t adv_data = {0};
    adv_data.set_scan_rsp = false;
    adv_data.include_name = true;
    adv_data.include_txpower = true;
    adv_data.min_interval = 0x0006; // 7.5ms
    adv_data.max_interval = 0x0010; // 10ms
    adv_data.appearance = 0x00;
    adv_data.manufacturer_len = 0;
    adv_data.p_manufacturer_data = NULL;
    adv_data.service_data_len = 0;
    adv_data.p_service_data = NULL;
    adv_data.service_uuid_len = 0;
    adv_data.p_service_uuid = NULL;
    adv_data.flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
    
    esp_ble_gap_config_adv_data(&adv_data);
    
    active_ = true;
    ESP_LOGI(TAG, "BLE configuration service started with advertising");
}

void BleConfig::Stop() {
    if (!active_) {
        return;
    }
    
    ESP_LOGI(TAG, "Stopping BLE configuration service");
    
    // Stop advertising
    esp_ble_gap_stop_advertising();
    
    // Unregister app
    esp_ble_gatts_app_unregister(ESP_APP_ID);
    
    // Disable Bluetooth
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    
    active_ = false;
    device_info_handle = 0;
    wifi_config_handle = 0;
    status_handle = 0;
    current_conn_id = 0;
    
    ESP_LOGI(TAG, "BLE configuration service stopped");
}

void BleConfig::SetDeviceInfo(const std::string& uuid, const std::string& mac) {
    device_uuid_ = uuid;
    device_mac_ = mac;
    
    // Create JSON for device info
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "uuid", device_uuid_.c_str());
    cJSON_AddStringToObject(root, "mac", device_mac_.c_str());
    cJSON_AddStringToObject(root, "firmware", "1.0.0");
    cJSON_AddStringToObject(root, "model", "xiaozhi-esp32");
    
    char* json_str = cJSON_PrintUnformatted(root);
    device_info_json = json_str;
    
    cJSON_free(json_str);
    cJSON_Delete(root);
    
    ESP_LOGI(TAG, "Device info set - UUID: %s, MAC: %s", uuid.c_str(), mac.c_str());
}

void BleConfig::SetConfigCallback(ConfigCallback callback) {
    config_callback_ = callback;
}

void BleConfig::NotifyStatus(ConfigStatus status, const std::string& message) {
    if (!active_ || current_conn_id == 0 || status_handle == 0) {
        return;
    }
    
    // Create status JSON
    cJSON* root = cJSON_CreateObject();
    
    const char* status_str = "waiting";
    switch (status) {
        case STATUS_CONNECTING: status_str = "connecting"; break;
        case STATUS_CONNECTED: status_str = "connected"; break;
        case STATUS_FAILED: status_str = "failed"; break;
        default: break;
    }
    
    cJSON_AddStringToObject(root, "status", status_str);
    cJSON_AddStringToObject(root, "message", message.c_str());
    
    char* json_str = cJSON_PrintUnformatted(root);
    
    // Send notification to connected device
    if (gl_profile_tab[PROFILE_APP_ID].gatts_if != ESP_GATT_IF_NONE) {
        esp_ble_gatts_send_indicate(
            gl_profile_tab[PROFILE_APP_ID].gatts_if,
            current_conn_id,
            status_handle,
            strlen(json_str),
            (uint8_t*)json_str,
            false  // notification = true, indication = false
        );
    }
    
    cJSON_free(json_str);
    cJSON_Delete(root);
}

void BleConfig::OnWifiConfigWrite(const std::string& value) {
    // This is now handled inline in the event handler
    ESP_LOGI(TAG, "OnWifiConfigWrite called with %d bytes", value.length());
}

std::string BleConfig::OnDeviceInfoRead() {
    return device_info_json;
}

#else // !CONFIG_BT_ENABLED

// Stub implementations when Bluetooth is not enabled

BleConfig& BleConfig::GetInstance() {
    static BleConfig instance;
    return instance;
}

BleConfig::BleConfig() {}
BleConfig::~BleConfig() {}

void BleConfig::Start(const std::string& deviceName) {
    ESP_LOGE(TAG, "BLE not enabled in menuconfig. Run 'idf.py menuconfig' and enable Bluetooth.");
}

void BleConfig::Stop() {}

void BleConfig::SetDeviceInfo(const std::string& uuid, const std::string& mac) {}

void BleConfig::SetConfigCallback(ConfigCallback callback) {}

void BleConfig::NotifyStatus(ConfigStatus status, const std::string& message) {}

void BleConfig::OnWifiConfigWrite(const std::string& value) {}

std::string BleConfig::OnDeviceInfoRead() {
    return "{}";
}

#endif // CONFIG_BT_ENABLED