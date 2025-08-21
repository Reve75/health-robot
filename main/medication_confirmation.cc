#include "medication_confirmation.h"
#include "application.h"
#include "audio_prompts/medication_prompt.h"
#include "protocols/protocol.h"
#include <esp_timer.h>

MedicationConfirmation::~MedicationConfirmation() {
    if (timeout_timer_ != nullptr) {
        xTimerDelete(timeout_timer_, 0);
        timeout_timer_ = nullptr;
    }
}

void MedicationConfirmation::Initialize(const std::string& user_id) {
    user_id_ = user_id;
    ESP_LOGI(TAG, "[MEDICATION] Initialized with user_id: %s", user_id_.c_str());
    ESP_LOGI(TAG, "[MEDICATION] Listening timeout set to %lu ms", LISTENING_TIMEOUT_MS);
}

void MedicationConfirmation::SendMedicationContext() {
    if (!protocol_) {
        ESP_LOGW(TAG, "[MEDICATION] Protocol not set, cannot send context");
        return;
    }
    
    ESP_LOGI(TAG, "[MEDICATION] Sending medication context to server for push-to-talk");
    
    // Use the existing protocol method
    protocol_->SendMedicationContext(user_id_);
    ESP_LOGI(TAG, "[MEDICATION] Context sent for user: %s", user_id_.c_str());
}

void MedicationConfirmation::StartConfirmation() {
    ESP_LOGI(TAG, "[MEDICATION] StartConfirmation called, current state: %d", (int)state_);
    
    if (state_ != State::kIdle) {
        ESP_LOGW(TAG, "[MEDICATION] Already in progress (state=%d), ignoring request", (int)state_);
        return;
    }
    
    ESP_LOGI(TAG, "[MEDICATION] Starting confirmation sequence");
    ESP_LOGI(TAG, "[MEDICATION] User ID: %s", user_id_.c_str());
    LogEvent("medication_prompted", "Button pressed");
    
    state_ = State::kPlayingPrompt;
    ESP_LOGI(TAG, "[MEDICATION] State changed to kPlayingPrompt");
    
    // Schedule prompt playback in main thread
    Application::GetInstance().Schedule([this]() {
        PlayPrompt();
    });
}

void MedicationConfirmation::PlayPrompt() {
    ESP_LOGI(TAG, "[MEDICATION] PlayPrompt called");
    ESP_LOGI(TAG, "[MEDICATION] Audio size: %zu bytes, duration: %.2f seconds", 
             MedicationPrompts::MEDICATION_PROMPT_SIZE, 
             MedicationPrompts::MEDICATION_PROMPT_DURATION);
    
    using namespace MedicationPrompts;
    
    // Convert header data to string_view for PlaySound
    std::string_view prompt_audio(
        reinterpret_cast<const char*>(MEDICATION_PROMPT_DATA),
        MEDICATION_PROMPT_SIZE
    );
    
    // Play the audio prompt
    Application::GetInstance().PlaySound(prompt_audio);
    
    // Wait for prompt to finish (approximately 2.4 seconds)
    // Then start listening for response
    Application::GetInstance().Schedule([this]() {
        // Small delay to ensure prompt finishes
        vTaskDelay(pdMS_TO_TICKS(2500));
        StartListening();
    });
}

void MedicationConfirmation::StartListening() {
    ESP_LOGI(TAG, "[MEDICATION] StartListening called");
    ESP_LOGI(TAG, "[MEDICATION] Previous state: %d", (int)state_);
    
    state_ = State::kRecording;
    ESP_LOGI(TAG, "[MEDICATION] State changed to kRecording");
    
    // Start timeout timer for 5 seconds
    if (timeout_timer_ == nullptr) {
        timeout_timer_ = xTimerCreate(
            "med_timeout",
            pdMS_TO_TICKS(LISTENING_TIMEOUT_MS),
            pdFALSE,  // One-shot timer
            this,
            OnTimeoutCallback
        );
    }
    
    if (xTimerStart(timeout_timer_, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start timeout timer");
        Reset();
        return;
    }
    
    // Send context message to server BEFORE starting to listen
    if (protocol_) {
        ESP_LOGI(TAG, "[MEDICATION] Sending medication context to server");
        protocol_->SendMedicationContext(user_id_);
    }
    
    // Use the existing StartListening - same as push-to-talk press
    ESP_LOGI(TAG, "[MEDICATION] Calling Application::StartListening (PTT mode)");
    Application::GetInstance().StartListening();
    
    ESP_LOGI(TAG, "[MEDICATION] Listening started, waiting for response (timeout: %lu ms)", LISTENING_TIMEOUT_MS);
}

// Removed OnVoiceActivityDetected and OnAudioData - we'll use the existing audio flow

void MedicationConfirmation::StopListening() {
    ESP_LOGI(TAG, "[MEDICATION] StopListening called, current state: %d", (int)state_);
    
    // Cancel timeout timer
    if (timeout_timer_ != nullptr) {
        xTimerStop(timeout_timer_, 0);
    }
    
    // Use the existing StopListening - same as push-to-talk release
    Application::GetInstance().StopListening();
    
    // The audio has already been sent to server via the normal flow
    // Now we just need to wait for the server response
    state_ = State::kProcessing;
}

// SendAudioToServer removed - audio flows through existing PTT mechanism

void MedicationConfirmation::OnServerResponse(const std::string& response, const std::string& transcription) {
    ESP_LOGI(TAG, "[MEDICATION] OnServerResponse called");
    ESP_LOGI(TAG, "[MEDICATION] Response: '%s'", response.c_str());
    ESP_LOGI(TAG, "[MEDICATION] Transcription: '%s'", transcription.c_str());
    ESP_LOGI(TAG, "[MEDICATION] Current state: %d", (int)state_);
    
    if (response == "yes") {
        ESP_LOGI(TAG, "[MEDICATION] YES response detected - user confirmed taking medication");
        ESP_LOGI(TAG, "[MEDICATION] User said: %s", transcription.c_str());
        LogEvent("medication_confirmed", transcription);
        ESP_LOGI(TAG, "[MEDICATION] Sending medication status to server (taken=true)");
        SendMedicationStatus(true);
        ESP_LOGI(TAG, "[MEDICATION] ✓ Medication confirmation complete - recorded as TAKEN");
        
    } else if (response == "no") {
        ESP_LOGI(TAG, "[MEDICATION] NO response detected - user declined medication");
        ESP_LOGI(TAG, "[MEDICATION] User said: %s", transcription.c_str());
        LogEvent("medication_declined", transcription);
        ESP_LOGI(TAG, "[MEDICATION] ✗ Medication confirmation complete - recorded as DECLINED");
        
    } else {
        ESP_LOGI(TAG, "[MEDICATION] UNCLEAR response: %s", response.c_str());
        ESP_LOGI(TAG, "[MEDICATION] Full transcription was: %s", transcription.c_str());
        LogEvent("medication_unclear", response + " - " + transcription);
        ESP_LOGI(TAG, "[MEDICATION] ? Medication confirmation complete - UNCLEAR response");
    }
    
    Reset();
}

void MedicationConfirmation::SendMedicationStatus(bool taken) {
    ESP_LOGI(TAG, "[MEDICATION] SendMedicationStatus called with taken=%d", taken);
    
    if (!protocol_) {
        ESP_LOGE(TAG, "[MEDICATION] ERROR: No protocol set for sending medication status");
        return;
    }
    
    ESP_LOGI(TAG, "[MEDICATION] Sending status to server: user_id=%s, taken=%d", user_id_.c_str(), taken);
    // Send medication status via protocol
    protocol_->SendMedicationStatus(user_id_, taken);
    ESP_LOGI(TAG, "[MEDICATION] Status sent successfully");
}

void MedicationConfirmation::LogEvent(const std::string& event, const std::string& details) {
    ESP_LOGI(TAG, "Event: %s - %s", event.c_str(), details.c_str());
    
    if (!protocol_) {
        return;
    }
    
    // Create log entry
    cJSON* log_entry = cJSON_CreateObject();
    if (!log_entry) {
        return;
    }
    
    cJSON_AddStringToObject(log_entry, "type", "log");
    cJSON_AddStringToObject(log_entry, "event", event.c_str());
    cJSON_AddStringToObject(log_entry, "details", details.c_str());
    cJSON_AddNumberToObject(log_entry, "timestamp", time(nullptr));
    cJSON_AddStringToObject(log_entry, "user_id", user_id_.c_str());
    
    char* json_str = cJSON_PrintUnformatted(log_entry);
    if (json_str) {
        // Send log via protocol (actual implementation)
        // protocol_->SendText(json_str);
        cJSON_free(json_str);
    }
    
    cJSON_Delete(log_entry);
}

void MedicationConfirmation::OnTimeout() {
    ESP_LOGI(TAG, "[MEDICATION] OnTimeout called - 5 second timeout reached");
    ESP_LOGI(TAG, "[MEDICATION] Current state: %d", (int)state_);
    
    if (state_ == State::kRecording) {
        ESP_LOGI(TAG, "[MEDICATION] Was in recording state, stopping listening now");
        // Stop listening after 5 seconds - same as releasing button
        StopListening();
        
        // Wait for server response
        LogEvent("medication_timeout", "5 second recording complete");
    } else {
        ESP_LOGW(TAG, "[MEDICATION] Timeout called but not in recording state (state=%d)", (int)state_);
    }
}

void MedicationConfirmation::Reset() {
    ESP_LOGI(TAG, "[MEDICATION] Reset called, previous state: %d", (int)state_);
    
    state_ = State::kIdle;
    ESP_LOGI(TAG, "[MEDICATION] State reset to kIdle");
    
    if (timeout_timer_ != nullptr) {
        ESP_LOGI(TAG, "[MEDICATION] Stopping and cleaning up timeout timer");
        xTimerStop(timeout_timer_, 0);
    }
}

// Static timer callback
void MedicationConfirmation::OnTimeoutCallback(TimerHandle_t timer) {
    MedicationConfirmation* instance = static_cast<MedicationConfirmation*>(pvTimerGetTimerID(timer));
    if (instance) {
        // Schedule timeout handling in main thread
        Application::GetInstance().Schedule([instance]() {
            instance->OnTimeout();
        });
    }
}