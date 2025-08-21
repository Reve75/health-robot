#include "websocket_protocol.h"
#include "board.h"
#include "system_info.h"
#include "application.h"
#include "settings.h"
#include "config/server_config.h"

#include <cstring>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <arpa/inet.h>
#include "assets/lang_config.h"

#define TAG "WS"

WebsocketProtocol::WebsocketProtocol() {
    event_group_handle_ = xEventGroupCreate();
}

WebsocketProtocol::~WebsocketProtocol() {
    StopPersistentConnection();
    if (websocket_ != nullptr) {
        delete websocket_;
    }
    vEventGroupDelete(event_group_handle_);
}

bool WebsocketProtocol::Start() {
    // Start persistent connection for control messages
    return StartPersistentConnection();
}

bool WebsocketProtocol::SendAudio(const AudioStreamPacket& packet) {
    if (websocket_ == nullptr) {
        return false;
    }

    if (version_ == 2) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol2) + packet.payload.size());
        auto bp2 = (BinaryProtocol2*)serialized.data();
        bp2->version = htons(version_);
        bp2->type = 0;
        bp2->reserved = 0;
        bp2->timestamp = htonl(packet.timestamp);
        bp2->payload_size = htonl(packet.payload.size());
        memcpy(bp2->payload, packet.payload.data(), packet.payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else if (version_ == 3) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol3) + packet.payload.size());
        auto bp3 = (BinaryProtocol3*)serialized.data();
        bp3->type = 0;
        bp3->reserved = 0;
        bp3->payload_size = htons(packet.payload.size());
        memcpy(bp3->payload, packet.payload.data(), packet.payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else {
        return websocket_->Send(packet.payload.data(), packet.payload.size(), true);
    }
}

bool WebsocketProtocol::SendText(const std::string& text) {
    // Try persistent connection first for control messages
    if (persistent_websocket_ != nullptr && persistent_connected_) {
        if (persistent_websocket_->Send(text)) {
            return true;
        }
    }
    
    // Fall back to audio channel if available
    if (websocket_ != nullptr) {
        if (!websocket_->Send(text)) {
            ESP_LOGE(TAG, "Failed to send text: %s", text.c_str());
            SetError(Lang::Strings::SERVER_ERROR);
            return false;
        }
        return true;
    }
    
    return false;
}

bool WebsocketProtocol::IsAudioChannelOpened() const {
    return websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_ && !IsTimeout();
}

void WebsocketProtocol::CloseAudioChannel() {
    if (websocket_ != nullptr) {
        ESP_LOGI(TAG, "Closing audio channel");
        delete websocket_;
        websocket_ = nullptr;
    }
    
}

bool WebsocketProtocol::OpenAudioChannel() {
    if (websocket_ != nullptr) {
        delete websocket_;
    }

    // Force check and update BEFORE loading
    // Load from configuration instead of hardcoded values
    Settings settings("websocket", false);
    std::string url = settings.GetString("url");
    std::string token = settings.GetString("token");
    int version = settings.GetInt("version");
    if (version != 0) {
        version_ = version;
    }
    
    // If no URL in settings or it's an old IP, use configured default
    if (url.empty() || url.find("192.168.8.157") != std::string::npos) {
        ESP_LOGW(TAG, "Invalid or old URL in settings, using configured default");
        url = ServerConfig::GetInstance().GetWebSocketUrl();
        // Save the new URL to settings
        Settings ws_fix("websocket", true);
        ws_fix.SetString("url", url);
        ws_fix.SetInt("version", 1);
        ESP_LOGI(TAG, "Updated WebSocket URL in NVS to: %s", url.c_str());
    }
    
    ESP_LOGI(TAG, "WebSocket settings loaded - URL: %s, Version: %d", url.c_str(), version_);

    error_occurred_ = false;

    websocket_ = Board::GetInstance().CreateWebSocket();
    
    if (!token.empty()) {
        // If token not has a space, add "Bearer " prefix
        if (token.find(" ") == std::string::npos) {
            token = "Bearer " + token;
        }
        websocket_->SetHeader("Authorization", token.c_str());
    }
    websocket_->SetHeader("Protocol-Version", std::to_string(version_).c_str());
    websocket_->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    websocket_->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    
    // Add User-Id header if configured (from BLE configuration)
    Settings device_settings("device", false);
    std::string user_id = device_settings.GetString("user_id");
    if (!user_id.empty()) {
        websocket_->SetHeader("User-Id", user_id.c_str());
        ESP_LOGI(TAG, "User-Id header set: %s", user_id.c_str());
    }

    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        if (binary) {
            if (on_incoming_audio_ != nullptr) {
                if (version_ == 2) {
                    BinaryProtocol2* bp2 = (BinaryProtocol2*)data;
                    bp2->version = ntohs(bp2->version);
                    bp2->type = ntohs(bp2->type);
                    bp2->timestamp = ntohl(bp2->timestamp);
                    bp2->payload_size = ntohl(bp2->payload_size);
                    auto payload = (uint8_t*)bp2->payload;
                    on_incoming_audio_(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = bp2->timestamp,
                        .payload = std::vector<uint8_t>(payload, payload + bp2->payload_size)
                    });
                } else if (version_ == 3) {
                    BinaryProtocol3* bp3 = (BinaryProtocol3*)data;
                    bp3->type = bp3->type;
                    bp3->payload_size = ntohs(bp3->payload_size);
                    auto payload = (uint8_t*)bp3->payload;
                    on_incoming_audio_(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>(payload, payload + bp3->payload_size)
                    });
                } else {
                    on_incoming_audio_(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>((uint8_t*)data, (uint8_t*)data + len)
                    });
                }
            }
        } else {
            // Parse JSON data
            auto root = cJSON_Parse(data);
            auto type = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type)) {
                if (strcmp(type->valuestring, "hello") == 0) {
                    ParseServerHello(root);
                } else {
                    if (on_incoming_json_ != nullptr) {
                        on_incoming_json_(root);
                    }
                }
            } else {
                ESP_LOGE(TAG, "Missing message type, data: %s", data);
            }
            cJSON_Delete(root);
        }
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    websocket_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "Websocket disconnected");
        if (on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
    });

    ESP_LOGI(TAG, "Connecting to websocket server: %s with version: %d", url.c_str(), version_);
    if (!websocket_->Connect(url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect to websocket server");
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    // Send hello message to describe the client
    auto message = GetHelloMessage();
    if (!SendText(message)) {
        return false;
    }

    // Wait for server hello
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    

    return true;
}

std::string WebsocketProtocol::GetHelloMessage() {
    // keys: message type, version, audio_params (format, sample_rate, channels)
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", version_);
    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
#if CONFIG_IOT_PROTOCOL_MCP
    cJSON_AddBoolToObject(features, "mcp", true);
#endif
    cJSON_AddItemToObject(root, "features", features);
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

void WebsocketProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (transport == nullptr || strcmp(transport->valuestring, "websocket") != 0) {
        ESP_LOGE(TAG, "Unsupported transport: %s", transport->valuestring);
        return;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate)) {
            server_sample_rate_ = sample_rate->valueint;
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration)) {
            server_frame_duration_ = frame_duration->valueint;
        }
    }

    xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
}

// ============== Persistent Connection Implementation ==============

bool WebsocketProtocol::StartPersistentConnection() {
    ESP_LOGI(TAG, "Starting persistent WebSocket connection...");
    
    if (persistent_websocket_ != nullptr) {
        delete persistent_websocket_;
        persistent_websocket_ = nullptr;
    }
    
    // Load WebSocket settings (reuse existing code)
    Settings settings("websocket", false);
    std::string url = settings.GetString("url");
    std::string token = settings.GetString("token");
    int version = settings.GetInt("version");
    if (version != 0) {
        version_ = version;
    }
    
    // Force update old IP if needed
    if (url.empty() || url.find("192.168.8.157") != std::string::npos) {
        url = ServerConfig::GetInstance().GetWebSocketUrl();
        Settings ws_fix("websocket", true);
        ws_fix.SetString("url", url);
        ESP_LOGI(TAG, "Updated persistent WebSocket URL to: %s", url.c_str());
    }
    
    ESP_LOGI(TAG, "Persistent WebSocket URL: %s", url.c_str());
    
    // Create persistent WebSocket
    persistent_websocket_ = Board::GetInstance().CreateWebSocket();
    
    // Set headers (reuse existing code)
    if (!token.empty()) {
        if (token.find(" ") == std::string::npos) {
            token = "Bearer " + token;
        }
        persistent_websocket_->SetHeader("Authorization", token.c_str());
    }
    persistent_websocket_->SetHeader("Protocol-Version", std::to_string(version_).c_str());
    persistent_websocket_->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    persistent_websocket_->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    
    // Add User-Id header if configured
    Settings device_settings("device", false);
    std::string user_id = device_settings.GetString("user_id");
    if (!user_id.empty()) {
        persistent_websocket_->SetHeader("User-Id", user_id.c_str());
    }
    
    // Set data handler for persistent connection
    persistent_websocket_->OnData([this](const char* data, size_t len, bool binary) {
        if (!binary) {
            HandlePersistentMessage(data, len);
        }
    });
    
    // Set connection handlers
    persistent_websocket_->OnConnected([this]() {
        ESP_LOGI(TAG, "Persistent WebSocket connected");
        persistent_connected_ = true;
        reconnect_attempts_ = 0;
        last_heartbeat_time_ = esp_timer_get_time() / 1000000;  // Convert to seconds
        
        // Send hello message
        std::string hello = GetHelloMessage();
        persistent_websocket_->Send(hello);
    });
    
    persistent_websocket_->OnDisconnected([this]() {
        ESP_LOGW(TAG, "Persistent WebSocket disconnected");
        persistent_connected_ = false;
        
        // Start reconnection task if not already running
        if (reconnect_task_ == nullptr) {
            xTaskCreate(ReconnectTask, "ws_reconnect", 4096, this, 5, &reconnect_task_);
        }
    });
    
    // Connect to server
    if (!persistent_websocket_->Connect(url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect persistent WebSocket");
        delete persistent_websocket_;
        persistent_websocket_ = nullptr;
        return false;
    }
    
    // Start heartbeat task
    if (heartbeat_task_ == nullptr) {
        xTaskCreate(HeartbeatTask, "ws_heartbeat", 2048, this, 5, &heartbeat_task_);
    }
    
    return true;
}

void WebsocketProtocol::StopPersistentConnection() {
    ESP_LOGI(TAG, "Stopping persistent WebSocket connection...");
    
    // Stop tasks
    if (heartbeat_task_ != nullptr) {
        vTaskDelete(heartbeat_task_);
        heartbeat_task_ = nullptr;
    }
    
    if (reconnect_task_ != nullptr) {
        vTaskDelete(reconnect_task_);
        reconnect_task_ = nullptr;
    }
    
    // Close persistent connection
    if (persistent_websocket_ != nullptr) {
        delete persistent_websocket_;
        persistent_websocket_ = nullptr;
    }
    
    persistent_connected_ = false;
}

bool WebsocketProtocol::SendHeartbeat() {
    if (persistent_websocket_ == nullptr || !persistent_connected_) {
        return false;
    }
    
    // Create heartbeat message
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "heartbeat");
    cJSON_AddStringToObject(root, "session_id", session_id_.c_str());
    cJSON_AddNumberToObject(root, "timestamp", esp_timer_get_time() / 1000);  // milliseconds
    
    auto json_str = cJSON_PrintUnformatted(root);
    bool result = persistent_websocket_->Send(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    
    if (result) {
        ESP_LOGD(TAG, "Heartbeat sent");
    } else {
        ESP_LOGW(TAG, "Failed to send heartbeat");
    }
    
    return result;
}

void WebsocketProtocol::HeartbeatTask(void* param) {
    WebsocketProtocol* self = (WebsocketProtocol*)param;
    const uint32_t HEARTBEAT_INTERVAL_SEC = 60;  // 1 minute
    
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_INTERVAL_SEC * 1000));
        
        if (self->persistent_connected_) {
            self->SendHeartbeat();
        }
    }
}

void WebsocketProtocol::ReconnectTask(void* param) {
    WebsocketProtocol* self = (WebsocketProtocol*)param;
    const uint32_t MAX_RECONNECT_DELAY_SEC = 60;  // Max 1 minute between attempts
    const uint32_t INITIAL_DELAY_SEC = 5;  // Start with 5 seconds
    
    while (!self->persistent_connected_) {
        self->reconnect_attempts_++;
        
        // Calculate delay with exponential backoff
        uint32_t delay = INITIAL_DELAY_SEC * (1 << (self->reconnect_attempts_ - 1));
        if (delay > MAX_RECONNECT_DELAY_SEC) {
            delay = MAX_RECONNECT_DELAY_SEC;
        }
        
        ESP_LOGI(TAG, "Reconnection attempt %lu in %lu seconds...", 
                 (unsigned long)self->reconnect_attempts_, (unsigned long)delay);
        vTaskDelay(pdMS_TO_TICKS(delay * 1000));
        
        if (!self->persistent_connected_) {
            ESP_LOGI(TAG, "Attempting to reconnect persistent WebSocket...");
            if (self->StartPersistentConnection()) {
                ESP_LOGI(TAG, "Reconnection successful");
                break;
            }
        }
    }
    
    self->reconnect_task_ = nullptr;
    vTaskDelete(nullptr);  // Delete this task
}

void WebsocketProtocol::HandlePersistentMessage(const char* data, size_t len) {
    // Parse JSON message
    auto root = cJSON_Parse(data);
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse persistent message: %s", data);
        return;
    }
    
    auto type = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type)) {
        const char* msg_type = type->valuestring;
        
        if (strcmp(msg_type, "hello") == 0) {
            // Handle server hello (reuse existing code)
            ParseServerHello(root);
        } else if (strcmp(msg_type, "heartbeat_ack") == 0) {
            ESP_LOGD(TAG, "Heartbeat acknowledged");
            last_heartbeat_time_ = esp_timer_get_time() / 1000000;
        } else {
            // Pass other messages to the application's JSON handler
            if (on_incoming_json_ != nullptr) {
                on_incoming_json_(root);
            }
        }
    }
    
    cJSON_Delete(root);
}

