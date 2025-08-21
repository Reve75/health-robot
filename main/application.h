#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <mutex>
#include <list>
#include <vector>
#include <condition_variable>
#include <memory>

#include <opus_encoder.h>
#include <opus_decoder.h>
#include <opus_resampler.h>

#include "protocol.h"
#include "ota.h"
#include "background_task.h"
#include "audio_processor.h"
#include "wake_word.h"
#include "audio_debugger.h"
#include "alarm_test_button.h"

#define SCHEDULE_EVENT (1 << 0)
#define SEND_AUDIO_EVENT (1 << 1)
#define CHECK_NEW_VERSION_DONE_EVENT (1 << 2)

enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

enum DeviceState {
    kDeviceStateUnknown,
    kDeviceStateStarting,
    kDeviceStateWifiConfiguring,
    kDeviceStateIdle,
    kDeviceStateConnecting,
    kDeviceStateListening,
    kDeviceStateSpeaking,
    kDeviceStateUpgrading,
    kDeviceStateActivating,
    kDeviceStateAudioTesting,
    kDeviceStateFatalError
};

#define OPUS_FRAME_DURATION_MS 60
#define MAX_AUDIO_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
#define AUDIO_TESTING_MAX_DURATION_MS 10000

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // 删除拷贝构造函数和赋值运算符
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void Start();
    DeviceState GetDeviceState() const { return device_state_; }
    bool IsVoiceDetected() const { return voice_detected_; }
    void Schedule(std::function<void()> callback);
    void SetDeviceState(DeviceState state);
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();
    void AbortSpeaking(AbortReason reason);
    void ToggleChatState();
    void StartListening(bool medication_context = false);
    void StopListening();
    void PlayMedicationPrompt();
    void UpdateIotStates();
    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    void PlaySound(const std::string_view& sound);
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void SetAecMode(AecMode mode);
    bool ReadAudio(std::vector<int16_t>& data, int sample_rate, int samples);
    AecMode GetAecMode() const { return aec_mode_; }
    BackgroundTask* GetBackgroundTask() const { return background_task_; }
    Protocol* GetProtocol() const { return protocol_.get(); }
    
    // Public alarm methods for testing
    bool SetAlarm(uint8_t hour, uint8_t minute);
    void StopAlarm();
    void ClearAllAlarms();
    
    // BLE configuration method
    void StartBleConfigMode();
    
    // Health monitoring methods
    void SaveBloodPressure(int systolic, int diastolic, int pulse = 0);
    void GetBloodPressure(int& systolic, int& diastolic, int& pulse) const;
    void SaveMedicationDetails(const std::string& name, const std::string& dosage, 
                               const std::string& frequency, int hour, int minute);
    bool HasAlarmSounded() const { return alarm_sounded_today_; }
    void MarkAlarmSounded();

private:
    Application();
    ~Application();

    std::unique_ptr<WakeWord> wake_word_;
    std::unique_ptr<AudioProcessor> audio_processor_;
    std::unique_ptr<AudioDebugger> audio_debugger_;
    std::mutex mutex_;
    std::list<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    volatile DeviceState device_state_ = kDeviceStateUnknown;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    bool medication_context_ = false;
    AecMode aec_mode_ = kAecOff;

    bool has_server_time_ = false;
    bool aborted_ = false;
    bool voice_detected_ = false;
    bool busy_decoding_audio_ = false;
    int clock_ticks_ = 0;
    TaskHandle_t check_new_version_task_handle_ = nullptr;
    
    // Button for BLE configuration (GPIO 48)
    std::unique_ptr<AlarmTestButton> ble_config_button_;

    // Audio encode / decode
    TaskHandle_t audio_loop_task_handle_ = nullptr;
    BackgroundTask* background_task_ = nullptr;
    std::chrono::steady_clock::time_point last_output_time_;
    std::list<AudioStreamPacket> audio_send_queue_;
    std::list<AudioStreamPacket> audio_decode_queue_;
    std::condition_variable audio_decode_cv_;
    std::list<AudioStreamPacket> audio_testing_queue_;

    // 新增：用于维护音频包的timestamp队列
    std::list<uint32_t> timestamp_queue_;
    std::mutex timestamp_mutex_;

    std::unique_ptr<OpusEncoderWrapper> opus_encoder_;
    std::unique_ptr<OpusDecoderWrapper> opus_decoder_;

    OpusResampler input_resampler_;
    OpusResampler reference_resampler_;
    OpusResampler output_resampler_;

    // Alarm clock functionality
    struct Alarm {
        uint8_t hour;      // 0-23
        uint8_t minute;    // 0-59
        bool enabled;      // Active flag
        bool triggered;    // Already fired today
    };
    std::vector<Alarm> alarms_;
    bool alarm_ringing_ = false;

    void MainEventLoop();
    void OnAudioInput();
    void OnAudioOutput();
    void ResetDecoder();
    void SetDecodeSampleRate(int sample_rate, int frame_duration);
    void CheckNewVersion(Ota& ota);
    void ShowActivationCode(const std::string& code, const std::string& message);
    void OnClockTimer();
    void SetListeningMode(ListeningMode mode, bool medication_context = false);
    void AudioLoop();
    void EnterAudioTestingMode();
    void ExitAudioTestingMode();
    
    // Alarm clock methods (private)
    void CheckAlarms();
    void TriggerAlarm();
    
    // User settings (persistent in NVS)
    std::string user_id_;
    std::string medication_name_;
    bool medication_taken_today_ = false;
    int med_reminder_hour_ = -1;
    int med_reminder_minute_ = -1;
    
    // Blood pressure data
    int bp_systolic_ = 0;
    int bp_diastolic_ = 0;
    
#ifdef CONFIG_USE_EYE_ANIMATION_STYLE
    // Eye animation variables (adapted from RoPet)
    bool is_blink = false;
    bool is_track = false;
    int16_t eyeNewX = 0;
    int16_t eyeNewY = 0;
    uint8_t eye_style_num = 7;  // Default eye style
    
    // Eye graphics data pointers
    const uint16_t *sclera = nullptr;
    const uint8_t *upper = nullptr;
    const uint8_t *lower = nullptr;
    const uint16_t *polar = nullptr;
    const uint16_t *iris = nullptr;
    
    // Eye animation methods
    void InitializeEyeAnimation();
    void EyeAnimationLoop();
    void SetEyeStyle(uint8_t style);
    void UpdateEyeExpression(const std::string& emotion);
    
    // Eye animation task handle
    TaskHandle_t eye_animation_task_handle_ = nullptr;
    
    // Eye blink timing
    uint32_t timeOfLastBlink = 0;
    uint32_t timeToNextBlink = 0;
#endif
    int bp_pulse_ = 0;
    time_t bp_timestamp_ = 0;
    
    // Enhanced medication details
    std::string medication_dosage_;
    std::string medication_frequency_;
    bool alarm_sounded_today_ = false;
    time_t last_sync_time_ = 0;
    
    // User settings methods
    void LoadUserSettings();
    void SaveAlarmToNVS(uint8_t hour, uint8_t minute);
    void ClearAlarmsFromNVS();
    void SaveMedicationInfo(const std::string& medication);
    void MarkMedicationTaken();
    void ResetDailyMedication();
    void ClearMedicationAndAlarms();  // Clear all medication data and alarms
};

#endif // _APPLICATION_H_
