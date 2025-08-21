// NimBLE-based BLE configuration implementation
// Uses Apache NimBLE stack which is lighter than Bluedroid

#include "sdkconfig.h"
#include "ble_config.h"

#include <assert.h>
#include <esp_log.h>
#include <cJSON.h>
#include <cstring>
#include <nvs_flash.h>

// NimBLE includes - these require CONFIG_BT_NIMBLE_ENABLED
#ifdef CONFIG_BT_NIMBLE_ENABLED

#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "host/ble_gap.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char* TAG = "BleConfig";

// UUIDs for our service and characteristics - using 16-bit UUIDs for simplicity
// WiFi Configuration Service UUID
#define WIFI_CONFIG_SVC_UUID    0x1820

// Characteristic UUIDs
#define DEVICE_INFO_CHR_UUID    0x2A00
#define WIFI_CONFIG_CHR_UUID    0x2A01
#define STATUS_CHR_UUID         0x2A02

// Create UUID structures
static const ble_uuid16_t gatt_svr_svc_uuid = BLE_UUID16_INIT(WIFI_CONFIG_SVC_UUID);
static const ble_uuid16_t gatt_svr_chr_device_info_uuid = BLE_UUID16_INIT(DEVICE_INFO_CHR_UUID);
static const ble_uuid16_t gatt_svr_chr_wifi_config_uuid = BLE_UUID16_INIT(WIFI_CONFIG_CHR_UUID);
static const ble_uuid16_t gatt_svr_chr_status_uuid = BLE_UUID16_INIT(STATUS_CHR_UUID);

// Static instance pointer
static BleConfig* s_instance = nullptr;

// Global variables following ESP32-NimbleBLE-For-Dummies pattern
static uint8_t own_addr_type;
static uint16_t conn_handle;
static uint16_t notification_handle;  // For future use if needed

// Device info storage - our addition
static std::string device_info_json;

// Forward declarations
static int gatt_svr_chr_access_device_info(uint16_t conn_handle, uint16_t attr_handle,
                                          struct ble_gatt_access_ctxt *ctxt, void *arg);
static int gatt_svr_chr_access_wifi_config(uint16_t conn_handle, uint16_t attr_handle,
                                          struct ble_gatt_access_ctxt *ctxt, void *arg);
static int gatt_svr_chr_access_status(uint16_t conn_handle, uint16_t attr_handle,
                                     struct ble_gatt_access_ctxt *ctxt, void *arg);

// GATT service definition
static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        // Service: WiFi Configuration
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &gatt_svr_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                // Characteristic: Device Info (Read)
                .uuid = &gatt_svr_chr_device_info_uuid.u,
                .access_cb = gatt_svr_chr_access_device_info,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                // Characteristic: WiFi Config (Write)
                .uuid = &gatt_svr_chr_wifi_config_uuid.u,
                .access_cb = gatt_svr_chr_access_wifi_config,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                // Characteristic: Status (Notify)
                .uuid = &gatt_svr_chr_status_uuid.u,
                .access_cb = gatt_svr_chr_access_status,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &notification_handle,  // Match their naming
            },
            {
                0, // No more characteristics
            }
        },
    },
    {
        0, // No more services
    },
};

// Device info characteristic read handler - simplified like ESP32-NimbleBLE-For-Dummies
static int gatt_svr_chr_access_device_info(uint16_t conn_handle, uint16_t attr_handle,
                                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    int rc;
    
    ESP_LOGI(TAG, ">>> Device info access - op: %d, handle: %d, conn: %d", 
             ctxt->op, attr_handle, conn_handle);
    
    switch (ctxt->op) {
        case BLE_GATT_ACCESS_OP_READ_CHR:
            ESP_LOGI(TAG, ">>> READ request for device info characteristic");
            
            if (device_info_json.empty()) {
                // Set a default value if empty
                const char* default_info = "{\"uuid\":\"unknown\",\"mac\":\"unknown\"}";
                ESP_LOGI(TAG, ">>> Returning default info: %s", default_info);
                rc = os_mbuf_append(ctxt->om, default_info, strlen(default_info));
            } else {
                ESP_LOGI(TAG, ">>> Returning device info: %s", device_info_json.c_str());
                rc = os_mbuf_append(ctxt->om, device_info_json.c_str(), device_info_json.length());
            }
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
            
        default:
            assert(0);
            return BLE_ATT_ERR_UNLIKELY;
    }
}

// WiFi config characteristic write handler - pattern from ESP32-NimbleBLE-For-Dummies
static int gatt_svr_chr_access_wifi_config(uint16_t conn_handle, uint16_t attr_handle,
                                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    int rc;
    
    ESP_LOGI(TAG, ">>> WiFi config access - op: %d, handle: %d, conn: %d",
             ctxt->op, attr_handle, conn_handle);
    
    switch (ctxt->op) {
        case BLE_GATT_ACCESS_OP_WRITE_CHR: {
            ESP_LOGI(TAG, ">>> WRITE request for WiFi config characteristic");
            // Read the data from mbuf
            uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
            uint8_t buf[512];
            uint16_t len = om_len < sizeof(buf) ? om_len : sizeof(buf) - 1;
            
            rc = ble_hs_mbuf_to_flat(ctxt->om, buf, len, &len);
            if (rc != 0) {
                return BLE_ATT_ERR_UNLIKELY;
            }
            
            buf[len] = '\0';
            ESP_LOGI(TAG, "Received WiFi config: %s", (char*)buf);
            
            // Parse JSON
            cJSON* root = cJSON_Parse((char*)buf);
            if (root && s_instance) {
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
                if (!creds.userId.empty() && !creds.ssid.empty() && s_instance->config_callback_) {
                    ESP_LOGI(TAG, "Valid credentials received - User: %s, SSID: %s", 
                             creds.userId.c_str(), creds.ssid.c_str());
                    s_instance->config_callback_(creds);
                }
            }
            
            return 0;
        }
        
        default:
            assert(0);
            return BLE_ATT_ERR_UNLIKELY;
    }
}

// Status characteristic handler (not used for one-time WiFi config)
static int gatt_svr_chr_access_status(uint16_t conn_handle, uint16_t attr_handle,
                                     struct ble_gatt_access_ctxt *ctxt, void *arg) {
    // Not needed for one-time WiFi configuration
    // Status updates will happen over WebSocket after WiFi connects
    return 0;
}

// GAP event handler - matching ESP32-NimbleBLE-For-Dummies bleprph_gap_event
static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    struct ble_gap_conn_desc desc;
    int rc;
    
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            /* A new connection was established or a connection attempt failed. */
            ESP_LOGI(TAG, ">>> GAP EVENT: connection %s; status=%d",
                    event->connect.status == 0 ? "established" : "failed",
                    event->connect.status);
            if (event->connect.status == 0) {
                rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
                assert(rc == 0);
                ESP_LOGI(TAG, ">>> Connection handle=%d, peer addr: %02x:%02x:%02x:%02x:%02x:%02x", 
                        desc.conn_handle,
                        desc.peer_id_addr.val[5], desc.peer_id_addr.val[4],
                        desc.peer_id_addr.val[3], desc.peer_id_addr.val[2],
                        desc.peer_id_addr.val[1], desc.peer_id_addr.val[0]);
            }
            
            if (event->connect.status != 0) {
                /* Connection failed; resume advertising. */
                BleConfig::GetInstance().StartAdvertising();
            }
            conn_handle = event->connect.conn_handle;
            return 0;
            
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, ">>> GAP EVENT: disconnect; reason=%d (0x%x)", 
                     event->disconnect.reason, event->disconnect.reason);
            
            /* Connection terminated; resume advertising. */
            BleConfig::GetInstance().StartAdvertising();
            return 0;
            
        case BLE_GAP_EVENT_ADV_COMPLETE:
            ESP_LOGI(TAG, "advertise complete; reason=%d",
                    event->adv_complete.reason);
            BleConfig::GetInstance().StartAdvertising();
            return 0;
            
        default:
            return 0;
    }
}

// NimBLE host task
static void ble_host_task(void *param) {
    ESP_LOGI(TAG, "BLE Host Task Started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// Sync callback - matches ESP32-NimbleBLE-For-Dummies pattern
static void ble_on_sync(void) {
    ESP_LOGI(TAG, "BLE on sync");
    
    // Ensure we have a valid address
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to ensure address: %d", rc);
    }
    
    // Figure out address to use while advertising (no privacy for now)
    rc = ble_hs_id_infer_auto(0, &own_addr_type);  // Store in global variable!
    if (rc != 0) {
        ESP_LOGE(TAG, "error determining address type; rc=%d", rc);
        return;
    }
    
    // Printing ADDR
    uint8_t addr_val[6] = {0};
    rc = ble_hs_id_copy_addr(own_addr_type, addr_val, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "Device Address: %02x:%02x:%02x:%02x:%02x:%02x",
                addr_val[5], addr_val[4], addr_val[3], 
                addr_val[2], addr_val[1], addr_val[0]);
    }
    
    // Log all registered services
    ESP_LOGI(TAG, ">>> Services registered:");
    ESP_LOGI(TAG, ">>> - GAP Service: 0x1800");
    ESP_LOGI(TAG, ">>> - GATT Service: 0x1801");  
    ESP_LOGI(TAG, ">>> - WiFi Config Service: 0x1820");
    ESP_LOGI(TAG, ">>>   - Device Info Char: 0x2A00 (handle 16)");
    ESP_LOGI(TAG, ">>>   - WiFi Config Char: 0x2A01 (handle 18)");
    ESP_LOGI(TAG, ">>>   - Status Char: 0x2A02 (handle 20)");
    
    // Begin advertising
    BleConfig::GetInstance().StartAdvertising();
}

// Reset callback
static void ble_on_reset(int reason) {
    ESP_LOGE(TAG, "BLE reset, reason: %d", reason);
}

// Forward declaration - not in headers by design
extern "C" void ble_store_config_init(void);

// Registration callback for GATT services
static void gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg) {
    char buf[BLE_UUID_STR_LEN];
    
    switch (ctxt->op) {
        case BLE_GATT_REGISTER_OP_SVC:
            ESP_LOGI(TAG, "Registered service %s with handle=%d",
                    ble_uuid_to_str(ctxt->svc.svc_def->uuid, buf),
                    ctxt->svc.handle);
            break;
            
        case BLE_GATT_REGISTER_OP_CHR:
            ESP_LOGI(TAG, "Registered characteristic %s with def_handle=%d val_handle=%d",
                    ble_uuid_to_str(ctxt->chr.chr_def->uuid, buf),
                    ctxt->chr.def_handle,
                    ctxt->chr.val_handle);
            break;
            
        case BLE_GATT_REGISTER_OP_DSC:
            ESP_LOGI(TAG, "Registered descriptor %s with handle=%d",
                    ble_uuid_to_str(ctxt->dsc.dsc_def->uuid, buf),
                    ctxt->dsc.handle);
            break;
            
        default:
            assert(0);
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

// Helper function matching ESP32-NimbleBLE-For-Dummies gatt_svr_init
static int gatt_svr_init(void) {
    int rc;
    
    ble_svc_gap_init();
    ble_svc_gatt_init();
    
    rc = ble_gatts_count_cfg(gatt_svr_svcs);
    if (rc != 0) {
        return rc;
    }
    
    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    if (rc != 0) {
        return rc;
    }
    
    return 0;
}

void BleConfig::Start(const std::string& deviceName) {
    if (active_) {
        ESP_LOGW(TAG, "BLE already active");
        return;
    }
    
    ESP_LOGI(TAG, "Starting BLE");
    int rc;
    
    // Initialize NVS — it is used to store PHY calibration data
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    
    // Initialize NimBLE port (this handles HCI and controller init in newer ESP-IDF)
    nimble_port_init();
    
    /* Initialize the NimBLE host configuration. */
    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.gatts_register_cb = gatt_svr_register_cb;
    // ble_hs_cfg.store_status_cb = ble_store_util_status_rr;  // Optional, works without it
    
    rc = gatt_svr_init();
    assert(rc == 0);
    
    /* Set the default device name. */
    rc = ble_svc_gap_device_name_set(deviceName.c_str());
    assert(rc == 0);
    
    /* XXX Need to have template for store */
    ble_store_config_init();
    
    // Set active before starting the thread
    active_ = true;
    
    nimble_port_freertos_init(ble_host_task);
    ESP_LOGI(TAG, "BLE started");
}

void BleConfig::StartAdvertising() {
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    const char *name;
    int rc;
    
    // Static array for UUID to avoid temporary array issue in C++
    static ble_uuid16_t adv_uuids[] = {
        BLE_UUID16_INIT(WIFI_CONFIG_SVC_UUID)
    };
    
    /**
     *  Set the advertisement data included in our advertisements:
     *     o Flags (indicates advertisement type and other general info).
     *     o Advertising tx power.
     *     o Device name.
     *     o 16-bit service UUIDs (alert notifications).
     */
    
    memset(&fields, 0, sizeof fields);
    
    /* Advertise two flags:
     *     o Discoverability in forthcoming advertisement (general)
     *     o BLE-only (BR/EDR unsupported).
     */
    fields.flags = BLE_HS_ADV_F_DISC_GEN |
                   BLE_HS_ADV_F_BREDR_UNSUP;
    
    /* Indicate that the TX power level field should be included; have the
     * stack fill this value automatically.  This is done by assigning the
     * special value BLE_HS_ADV_TX_PWR_LVL_AUTO.
     */
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    
    name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    
    fields.uuids16 = adv_uuids;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    
    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "error setting advertisement data; rc=%d", rc);
        return;
    }
    
    /* Begin advertising. */
    memset(&adv_params, 0, sizeof adv_params);
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "error enabling advertisement; rc=%d", rc);
        return;
    }
}

void BleConfig::Stop() {
    if (!active_) {
        return;
    }
    
    ESP_LOGI(TAG, "Stopping NimBLE configuration service");
    
    // Stop advertising
    ble_gap_adv_stop();
    
    // NimBLE cleanup is handled by the host task
    
    active_ = false;
    ESP_LOGI(TAG, "NimBLE configuration service stopped");
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
    // For one-time WiFi configuration, we don't need BLE notifications
    // Status updates will be handled via WebSocket after WiFi connects
    // This function is kept for compatibility but does nothing
    ESP_LOGI(TAG, "Status: %s - %s", 
             status == STATUS_CONNECTING ? "connecting" :
             status == STATUS_CONNECTED ? "connected" :
             status == STATUS_FAILED ? "failed" : "waiting",
             message.c_str());
}

void BleConfig::OnWifiConfigWrite(const std::string& value) {
    // Handled inline in the characteristic handler
    ESP_LOGI(TAG, "OnWifiConfigWrite called with %d bytes", value.length());
}

std::string BleConfig::OnDeviceInfoRead() {
    return device_info_json;
}

// Store configuration function removed - using the one from ble_store_config.h

#else // !CONFIG_BT_NIMBLE_ENABLED

// Stub implementations when NimBLE is not enabled

static const char* TAG = "BleConfig";

BleConfig& BleConfig::GetInstance() {
    static BleConfig instance;
    return instance;
}

BleConfig::BleConfig() {}
BleConfig::~BleConfig() {}

void BleConfig::Start(const std::string& deviceName) {
    ESP_LOGE(TAG, "NimBLE not enabled in menuconfig. Run 'idf.py menuconfig' and enable NimBLE.");
}

void BleConfig::StartAdvertising() {}

void BleConfig::Stop() {}

void BleConfig::SetDeviceInfo(const std::string& uuid, const std::string& mac) {}

void BleConfig::SetConfigCallback(ConfigCallback callback) {}

void BleConfig::NotifyStatus(ConfigStatus status, const std::string& message) {}

void BleConfig::OnWifiConfigWrite(const std::string& value) {}

std::string BleConfig::OnDeviceInfoRead() {
    return "{}";
}

#endif // CONFIG_BT_NIMBLE_ENABLED