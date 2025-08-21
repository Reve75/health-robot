#ifndef _MEDICATION_CONFIRMATION_H_
#define _MEDICATION_CONFIRMATION_H_

#include <string>
#include <memory>
#include <vector>
#include <functional>
#include <chrono>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#include <esp_log.h>

// Forward declarations
class Protocol;

class MedicationConfirmation {
public:
    static MedicationConfirmation& GetInstance() {
        static MedicationConfirmation instance;
        return instance;
    }
    
    // Delete copy constructor and assignment operator
    MedicationConfirmation(const MedicationConfirmation&) = delete;
    MedicationConfirmation& operator=(const MedicationConfirmation&) = delete;
    
    // Initialize with user information
    void Initialize(const std::string& user_id);
    
    // Start confirmation process (called on button press)
    void StartConfirmation();
    
    // Send medication context to server (for push-to-talk testing)
    void SendMedicationContext();
    
    // Process server response
    void OnServerResponse(const std::string& response, const std::string& transcription = "");
    
    // Set protocol for server communication
    void SetProtocol(Protocol* protocol) { protocol_ = protocol; }
    
    // Check if confirmation is in progress
    bool IsActive() const { return state_ != State::kIdle; }
    
private:
    enum class State {
        kIdle,
        kPlayingPrompt,
        kWaitingForVoice,
        kRecording,
        kProcessing
    };
    
    MedicationConfirmation() = default;
    ~MedicationConfirmation();
    
    // Internal methods
    void PlayPrompt();
    void StartListening();
    void StopListening();
    void SendAudioToServer();
    void SendMedicationStatus(bool taken);
    void LogEvent(const std::string& event, const std::string& details);
    void OnTimeout();
    void Reset();
    
    // State management
    State state_ = State::kIdle;
    std::string user_id_;
    
    // Timing
    TimerHandle_t timeout_timer_ = nullptr;
    std::chrono::steady_clock::time_point listening_start_time_;
    
    // Protocol for server communication
    Protocol* protocol_ = nullptr;
    
    // Callbacks
    std::function<void(bool)> vad_callback_backup_;
    
    // Constants
    static constexpr uint32_t LISTENING_TIMEOUT_MS = 5000;  // 5 seconds as per updated spec
    static constexpr uint32_t MIN_VOICE_DURATION_MS = 500;
    static constexpr uint32_t MAX_RECORDING_MS = 3000;
    static constexpr const char* TAG = "MedicationConfirm";
    
    // Static timer callback
    static void OnTimeoutCallback(TimerHandle_t timer);
};

#endif // _MEDICATION_CONFIRMATION_H_