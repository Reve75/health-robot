#include "protocol.h"

#include <esp_log.h>
#include <time.h>

#define TAG "Protocol"

void Protocol::OnIncomingJson(std::function<void(const cJSON* root)> callback) {
    on_incoming_json_ = callback;
}

void Protocol::OnIncomingAudio(std::function<void(AudioStreamPacket&& packet)> callback) {
    on_incoming_audio_ = callback;
}

void Protocol::OnAudioChannelOpened(std::function<void()> callback) {
    on_audio_channel_opened_ = callback;
}

void Protocol::OnAudioChannelClosed(std::function<void()> callback) {
    on_audio_channel_closed_ = callback;
}

void Protocol::OnNetworkError(std::function<void(const std::string& message)> callback) {
    on_network_error_ = callback;
}

void Protocol::SetError(const std::string& message) {
    error_occurred_ = true;
    if (on_network_error_ != nullptr) {
        on_network_error_(message);
    }
}

void Protocol::SendAbortSpeaking(AbortReason reason) {
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"abort\"";
    if (reason == kAbortReasonWakeWordDetected) {
        message += ",\"reason\":\"wake_word_detected\"";
    }
    message += "}";
    SendText(message);
}

void Protocol::SendWakeWordDetected(const std::string& wake_word) {
    std::string json = "{\"session_id\":\"" + session_id_ + 
                      "\",\"type\":\"listen\",\"state\":\"detect\",\"text\":\"" + wake_word + "\"}";
    SendText(json);
}

void Protocol::SendStartListening(ListeningMode mode, bool medication_context) {
    std::string message = "{\"session_id\":\"" + session_id_ + "\"";
    message += ",\"type\":\"listen\",\"state\":\"start\"";
    if (mode == kListeningModeRealtime) {
        message += ",\"mode\":\"realtime\"";
    } else if (mode == kListeningModeAutoStop) {
        message += ",\"mode\":\"auto\"";
    } else {
        message += ",\"mode\":\"manual\"";
    }
    if (medication_context) {
        message += ",\"medication_context\":true";
    }
    message += "}";
    SendText(message);
}

void Protocol::SendStopListening() {
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"listen\",\"state\":\"stop\"}";
    SendText(message);
}

void Protocol::SendMedicationCleared() {
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"medication_cleared\",\"status\":\"success\"}";
    SendText(message);
}

void Protocol::SendMedicationTaken(const std::string& medication_name) {
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"medication_taken\",\"medication\":\"" + medication_name + "\"}";
    SendText(message);
}

void Protocol::SendBloodPressure(int systolic, int diastolic, int pulse) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "session_id", session_id_.c_str());
    cJSON_AddStringToObject(root, "type", "bp_reading");
    cJSON_AddNumberToObject(root, "systolic", systolic);
    cJSON_AddNumberToObject(root, "diastolic", diastolic);
    if (pulse > 0) {
        cJSON_AddNumberToObject(root, "pulse", pulse);
    }
    
    char* json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        SendText(json_str);
        cJSON_free(json_str);
    }
    cJSON_Delete(root);
}

void Protocol::SendMedicationStatus(bool taken, const std::string& method) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "session_id", session_id_.c_str());
    cJSON_AddStringToObject(root, "type", "medication_status");
    cJSON_AddBoolToObject(root, "taken", taken);
    cJSON_AddNumberToObject(root, "timestamp", (double)time(nullptr));
    cJSON_AddStringToObject(root, "method", method.c_str());
    
    char* json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        SendText(json_str);
        cJSON_free(json_str);
    }
    cJSON_Delete(root);
}

void Protocol::SendRequestMedication() {
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"get_medication\"}";
    SendText(message);
}

void Protocol::SendReminderSent(const std::string& reminder_type) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "session_id", session_id_.c_str());
    cJSON_AddStringToObject(root, "type", "reminder_sent");
    cJSON_AddStringToObject(root, "reminder_type", reminder_type.c_str());
    
    char* json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        SendText(json_str);
        cJSON_free(json_str);
    }
    cJSON_Delete(root);
}

void Protocol::SendIotDescriptors(const std::string& descriptors) {
    cJSON* root = cJSON_Parse(descriptors.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse IoT descriptors: %s", descriptors.c_str());
        return;
    }

    if (!cJSON_IsArray(root)) {
        ESP_LOGE(TAG, "IoT descriptors should be an array");
        cJSON_Delete(root);
        return;
    }

    int arraySize = cJSON_GetArraySize(root);
    for (int i = 0; i < arraySize; ++i) {
        cJSON* descriptor = cJSON_GetArrayItem(root, i);
        if (descriptor == nullptr) {
            ESP_LOGE(TAG, "Failed to get IoT descriptor at index %d", i);
            continue;
        }

        cJSON* messageRoot = cJSON_CreateObject();
        cJSON_AddStringToObject(messageRoot, "session_id", session_id_.c_str());
        cJSON_AddStringToObject(messageRoot, "type", "iot");
        cJSON_AddBoolToObject(messageRoot, "update", true);

        cJSON* descriptorArray = cJSON_CreateArray();
        cJSON_AddItemToArray(descriptorArray, cJSON_Duplicate(descriptor, 1));
        cJSON_AddItemToObject(messageRoot, "descriptors", descriptorArray);

        char* message = cJSON_PrintUnformatted(messageRoot);
        if (message == nullptr) {
            ESP_LOGE(TAG, "Failed to print JSON message for IoT descriptor at index %d", i);
            cJSON_Delete(messageRoot);
            continue;
        }

        SendText(std::string(message));
        cJSON_free(message);
        cJSON_Delete(messageRoot);
    }

    cJSON_Delete(root);
}

void Protocol::SendIotStates(const std::string& states) {
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"iot\",\"update\":true,\"states\":" + states + "}";
    SendText(message);
}

void Protocol::SendMcpMessage(const std::string& payload) {
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"mcp\",\"payload\":" + payload + "}";
    SendText(message);
}

bool Protocol::IsTimeout() const {
    const int kTimeoutSeconds = 120;
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_incoming_time_);
    bool timeout = duration.count() > kTimeoutSeconds;
    if (timeout) {
        ESP_LOGE(TAG, "Channel timeout %ld seconds", (long)duration.count());
    }
    return timeout;
}

void Protocol::SendMedicationContext() {
    // Send medication context message - server will use device_id from connection headers
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"medication_context\"}";
    SendText(message);
}
