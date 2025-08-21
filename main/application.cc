#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "ml307_ssl_transport.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "font_awesome_symbols.h"
#include "iot/thing_manager.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "audio_debugger.h"
#include "alarm_test_button.h"
#include "settings.h"
#include "boards/common/wifi_board.h"
#include "config/server_config.h"

#if CONFIG_USE_AUDIO_PROCESSOR
#include "afe_audio_processor.h"
#else
#include "no_audio_processor.h"
#endif

#if CONFIG_USE_AFE_WAKE_WORD
#include "afe_wake_word.h"
#elif CONFIG_USE_ESP_WAKE_WORD
#include "esp_wake_word.h"
#else
#include "no_wake_word.h"
#endif

#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>

#ifdef CONFIG_USE_EYE_ANIMATION_STYLE
// Include eye animation data based on display size
#if DISPLAY_WIDTH == 240
#include "eye_data/240_240/common.h"
#include "eye_data/240_240/default.h"
#include "eye_data/240_240/sclera_common.h"
#else
// For 128x32 OLEDs, we'll still use the 240x240 data but it will be cropped
#include "eye_data/240_240/common.h"
#include "eye_data/240_240/default.h"
#include "eye_data/240_240/sclera_common.h"
#endif
#endif

#define TAG "Application"


static const char* const STATE_STRINGS[] = {
    "unknown",
    "starting",
    "configuring",
    "idle",
    "connecting",
    "listening",
    "speaking",
    "upgrading",
    "activating",
    "audio_testing",
    "fatal_error",
    "invalid_state"
};

Application::Application() {
    event_group_ = xEventGroupCreate();
    background_task_ = new BackgroundTask(4096 * 7);

#if CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

#if CONFIG_USE_AUDIO_PROCESSOR
    audio_processor_ = std::make_unique<AfeAudioProcessor>();
#else
    audio_processor_ = std::make_unique<NoAudioProcessor>();
#endif

#if CONFIG_USE_AFE_WAKE_WORD
    wake_word_ = std::make_unique<AfeWakeWord>();
#elif CONFIG_USE_ESP_WAKE_WORD
    wake_word_ = std::make_unique<EspWakeWord>();
#else
    wake_word_ = std::make_unique<NoWakeWord>();
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            app->OnClockTimer();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    if (background_task_ != nullptr) {
        delete background_task_;
    }
    vEventGroupDelete(event_group_);
}

void Application::CheckNewVersion(Ota& ota) {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // 初始重试延迟为10秒

    while (true) {
        SetDeviceState(kDeviceStateActivating);
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        if (!ota.CheckVersion()) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char buffer[128];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, ota.GetCheckVersionUrl().c_str());
            Alert(Lang::Strings::ERROR, buffer, "sad", Lang::Sounds::P3_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (device_state_ == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // 每次重试后延迟时间翻倍
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // 重置重试延迟时间

        if (ota.HasNewVersion()) {
            Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "happy", Lang::Sounds::P3_UPGRADE);

            vTaskDelay(pdMS_TO_TICKS(3000));

            SetDeviceState(kDeviceStateUpgrading);
            
            display->SetIcon(FONT_AWESOME_DOWNLOAD);
            std::string message = std::string(Lang::Strings::NEW_VERSION) + ota.GetFirmwareVersion();
            display->SetChatMessage("system", message.c_str());

            auto& board = Board::GetInstance();
            board.SetPowerSaveMode(false);
            wake_word_->StopDetection();
            // 预先关闭音频输出，避免升级过程有音频操作
            auto codec = board.GetAudioCodec();
            codec->EnableInput(false);
            codec->EnableOutput(false);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                audio_decode_queue_.clear();
            }
            background_task_->WaitForCompletion();
            delete background_task_;
            background_task_ = nullptr;
            vTaskDelay(pdMS_TO_TICKS(1000));

            ota.StartUpgrade([display](int progress, size_t speed) {
                char buffer[64];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                display->SetChatMessage("system", buffer);
            });

            // If upgrade success, the device will reboot and never reach here
            display->SetStatus(Lang::Strings::UPGRADE_FAILED);
            ESP_LOGI(TAG, "Firmware upgrade failed...");
            vTaskDelay(pdMS_TO_TICKS(3000));
            Reboot();
            return;
        }

        // No new version, mark the current version as valid
        ota.MarkCurrentVersionValid();
        if (!ota.HasActivationCode() && !ota.HasActivationChallenge()) {
            xEventGroupSetBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT);
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota.HasActivationCode()) {
            ShowActivationCode(ota.GetActivationCode(), ota.GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota.Activate();
            if (err == ESP_OK) {
                xEventGroupSetBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT);
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (device_state_ == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::P3_0},
        digit_sound{'1', Lang::Sounds::P3_1}, 
        digit_sound{'2', Lang::Sounds::P3_2},
        digit_sound{'3', Lang::Sounds::P3_3},
        digit_sound{'4', Lang::Sounds::P3_4},
        digit_sound{'5', Lang::Sounds::P3_5},
        digit_sound{'6', Lang::Sounds::P3_6},
        digit_sound{'7', Lang::Sounds::P3_7},
        digit_sound{'8', Lang::Sounds::P3_8},
        digit_sound{'9', Lang::Sounds::P3_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "happy", Lang::Sounds::P3_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert %s: %s [%s]", status, message, emotion);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        ResetDecoder();
        PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (device_state_ == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::PlaySound(const std::string_view& sound) {
    // Wait for the previous sound to finish
    {
        std::unique_lock<std::mutex> lock(mutex_);
        audio_decode_cv_.wait(lock, [this]() {
            return audio_decode_queue_.empty();
        });
    }
    background_task_->WaitForCompletion();

    const char* data = sound.data();
    size_t size = sound.size();
    
    for (const char* p = data; p < data + size; ) {
        auto p3 = (BinaryProtocol3*)p;
        p += sizeof(BinaryProtocol3);

        auto payload_size = ntohs(p3->payload_size);
        AudioStreamPacket packet;
        packet.sample_rate = 16000;
        packet.frame_duration = 60;
        packet.payload.resize(payload_size);
        memcpy(packet.payload.data(), p3->payload, payload_size);
        p += payload_size;

        std::lock_guard<std::mutex> lock(mutex_);
        audio_decode_queue_.emplace_back(std::move(packet));
    }
}

void Application::EnterAudioTestingMode() {
    ESP_LOGI(TAG, "Entering audio testing mode");
    ResetDecoder();
    SetDeviceState(kDeviceStateAudioTesting);
}

void Application::ExitAudioTestingMode() {
    ESP_LOGI(TAG, "Exiting audio testing mode");
    SetDeviceState(kDeviceStateWifiConfiguring);
    // Copy audio_testing_queue_ to audio_decode_queue_
    std::lock_guard<std::mutex> lock(mutex_);
    audio_decode_queue_ = std::move(audio_testing_queue_);
    audio_decode_cv_.notify_all();
}

void Application::ToggleChatState() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        EnterAudioTestingMode();
        return;
    } else if (device_state_ == kDeviceStateAudioTesting) {
        ExitAudioTestingMode();
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime, false);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {
        Schedule([this]() {
            protocol_->CloseAudioChannel();
        });
    }
}

void Application::StartListening(bool medication_context) {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        EnterAudioTestingMode();
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (device_state_ == kDeviceStateIdle) {
        Schedule([this, medication_context]() {
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(kListeningModeAutoStop, medication_context);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this, medication_context]() {
            AbortSpeaking(kAbortReasonNone);
            SetListeningMode(kListeningModeAutoStop, medication_context);
        });
    }
}

void Application::PlayMedicationPrompt() {
    ESP_LOGI(TAG, "[MEDICATION] Playing medication prompt immediately");
    
    // Use the embedded P3 file
    extern const char p3_medication_prompt_start[] asm("_binary_medication_prompt_p3_start");
    extern const char p3_medication_prompt_end[] asm("_binary_medication_prompt_p3_end");
    
    const std::string_view medication_sound {
        static_cast<const char*>(p3_medication_prompt_start),
        static_cast<size_t>(p3_medication_prompt_end - p3_medication_prompt_start)
    };
    
    // Add audio directly to queue without waiting - plays immediately
    // This avoids the delay that causes audio cutoff
    const char* data = medication_sound.data();
    size_t size = medication_sound.size();
    
    for (const char* p = data; p < data + size; ) {
        auto p3 = (BinaryProtocol3*)p;
        p += sizeof(BinaryProtocol3);

        auto payload_size = ntohs(p3->payload_size);
        AudioStreamPacket packet;
        packet.sample_rate = 16000;
        packet.frame_duration = 60;
        packet.payload.resize(payload_size);
        memcpy(packet.payload.data(), p3->payload, payload_size);
        p += payload_size;

        std::lock_guard<std::mutex> lock(mutex_);
        audio_decode_queue_.emplace_back(std::move(packet));
    }
    
    ESP_LOGI(TAG, "[MEDICATION] Audio queued for immediate playback");
}

// This function is no longer needed - the button handler in compact_wifi_board.cc
// directly calls PlayMedicationPrompt() and schedules StartListening()

void Application::StopListening() {
    if (device_state_ == kDeviceStateAudioTesting) {
        ExitAudioTestingMode();
        return;
    }

    const std::array<int, 3> valid_states = {
        kDeviceStateListening,
        kDeviceStateSpeaking,
        kDeviceStateIdle,
    };
    // If not valid, do nothing
    if (std::find(valid_states.begin(), valid_states.end(), device_state_) == valid_states.end()) {
        return;
    }

    Schedule([this]() {
        if (device_state_ == kDeviceStateListening) {
            protocol_->SendStopListening();
            SetDeviceState(kDeviceStateIdle);
        }
    });
}

void Application::Start() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);
    
#ifdef CONFIG_USE_EYE_ANIMATION_STYLE
    // Initialize eye animation if enabled
    InitializeEyeAnimation();
#endif
    
    // Check and fix WebSocket URL early in startup
    ESP_LOGI(TAG, "Checking WebSocket URL configuration...");
    {
        Settings ws_check("websocket", false);  // Read-only first
        std::string current_url = ws_check.GetString("url");
        ESP_LOGI(TAG, "Current WebSocket URL in NVS: %s", current_url.c_str());
        
        if (current_url.empty() || current_url.find("192.168.8.157") != std::string::npos) {
            ESP_LOGW(TAG, "Invalid or old server URL detected! Updating from configuration...");
            Settings ws_fix("websocket", true);  // Read-write mode
            std::string configured_url = ServerConfig::GetInstance().GetWebSocketUrl();
            ws_fix.SetString("url", configured_url);
            ws_fix.SetInt("version", 1);
            ESP_LOGI(TAG, "WebSocket URL updated to: %s", configured_url.c_str());
            
            // Verify the update
            Settings ws_verify("websocket", false);
            std::string new_url = ws_verify.GetString("url");
            ESP_LOGI(TAG, "Verification - WebSocket URL is now: %s", new_url.c_str());
        }
    }
    
    // Initialize BLE config button early (GPIO 48)
    ESP_LOGI(TAG, "Initializing BLE config button on GPIO 48");
    ble_config_button_ = std::make_unique<AlarmTestButton>(GPIO_NUM_48);
    ESP_LOGI(TAG, "BLE config button initialized - Long press (2s) to start BLE config mode");

    /* Setup the display */
    auto display = board.GetDisplay();

    /* Setup the audio codec */
    auto codec = board.GetAudioCodec();
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(codec->output_sample_rate(), 1, OPUS_FRAME_DURATION_MS);
    opus_encoder_ = std::make_unique<OpusEncoderWrapper>(16000, 1, OPUS_FRAME_DURATION_MS);
    opus_encoder_->SetComplexity(0);
    if (aec_mode_ != kAecOff) {
        ESP_LOGI(TAG, "AEC mode: %d, setting opus encoder complexity to 0", aec_mode_);
        opus_encoder_->SetComplexity(0);
    } else {
#if CONFIG_USE_AUDIO_PROCESSOR
        ESP_LOGI(TAG, "Audio processor detected, setting opus encoder complexity to 5");
        opus_encoder_->SetComplexity(5);
#else
        ESP_LOGI(TAG, "Audio processor not detected, setting opus encoder complexity to 0");
        opus_encoder_->SetComplexity(0);
#endif
    }

    if (codec->input_sample_rate() != 16000) {
        input_resampler_.Configure(codec->input_sample_rate(), 16000);
        reference_resampler_.Configure(codec->input_sample_rate(), 16000);
    }
    codec->Start();

#if CONFIG_USE_AUDIO_PROCESSOR
    xTaskCreatePinnedToCore([](void* arg) {
        Application* app = (Application*)arg;
        app->AudioLoop();
        vTaskDelete(NULL);
    }, "audio_loop", 4096 * 2, this, 8, &audio_loop_task_handle_, 1);
#else
    xTaskCreate([](void* arg) {
        Application* app = (Application*)arg;
        app->AudioLoop();
        vTaskDelete(NULL);
    }, "audio_loop", 4096 * 2, this, 8, &audio_loop_task_handle_);
#endif

    /* Start the clock timer to update the status bar */
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    /* Wait for the network to be ready */
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);

    // Check for new firmware version or get the MQTT broker address
    Ota ota;
    CheckNewVersion(ota);

    // Initialize the protocol
    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    // Add MCP common tools before initializing the protocol
#if CONFIG_IOT_PROTOCOL_MCP
    McpServer::GetInstance().AddCommonTools();
#endif

    if (ota.HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota.HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnNetworkError([this](const std::string& message) {
        SetDeviceState(kDeviceStateIdle);
        Alert(Lang::Strings::ERROR, message.c_str(), "sad", Lang::Sounds::P3_EXCLAMATION);
    });
    protocol_->OnIncomingAudio([this](AudioStreamPacket&& packet) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (device_state_ == kDeviceStateSpeaking && audio_decode_queue_.size() < MAX_AUDIO_PACKETS_IN_QUEUE) {
            audio_decode_queue_.emplace_back(std::move(packet));
        }
    });
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveMode(false);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }

#if CONFIG_IOT_PROTOCOL_XIAOZHI
        auto& thing_manager = iot::ThingManager::GetInstance();
        protocol_->SendIotDescriptors(thing_manager.GetDescriptorsJson());
        std::string states;
        if (thing_manager.GetStatesJson(states, false)) {
            protocol_->SendIotStates(states);
        }
#endif
    });
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveMode(true);
        Schedule([this]() {
            // Always play TIMEOUT_002 when connection closes due to timeout
            // Server now disconnects after 10s of no activity without warning
            PlaySound(Lang::Sounds::P3_TIMEOUT_002);
            
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    if (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateListening) {
                        SetDeviceState(kDeviceStateSpeaking);
                    }
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    background_task_->WaitForCompletion();
                    if (device_state_ == kDeviceStateSpeaking) {
                        // Wait for audio queue to empty before switching states
                        // This ensures all audio chunks are processed
                        {
                            std::unique_lock<std::mutex> lock(mutex_);
                            if (!audio_decode_queue_.empty()) {
                                // Wait up to 2 seconds for queue to empty
                                audio_decode_cv_.wait_for(lock, std::chrono::seconds(2), [this]() {
                                    return audio_decode_queue_.empty();
                                });
                            }
                        }
                        
                        // Small delay for codec buffer to flush (50ms)
                        // This is much shorter than 500ms and only for hardware buffer
                        vTaskDelay(pdMS_TO_TICKS(50));
                        
                        // With 5-second timeout, always go back to listening
                        // The timeout will handle returning to idle if no activity
                        SetDeviceState(kDeviceStateListening);
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([this, display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([this, display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
#if CONFIG_IOT_PROTOCOL_MCP
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
#endif
#if CONFIG_IOT_PROTOCOL_XIAOZHI
        } else if (strcmp(type->valuestring, "iot") == 0) {
            auto commands = cJSON_GetObjectItem(root, "commands");
            if (cJSON_IsArray(commands)) {
                auto& thing_manager = iot::ThingManager::GetInstance();
                for (int i = 0; i < cJSON_GetArraySize(commands); ++i) {
                    auto command = cJSON_GetArrayItem(commands, i);
                    thing_manager.Invoke(command);
                }
            }
#endif
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::P3_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
        } else if (strcmp(type->valuestring, "medication_response") == 0) {
            // Handle medication confirmation response from server
            auto response = cJSON_GetObjectItem(root, "response");
            auto transcription = cJSON_GetObjectItem(root, "transcription");
            
            if (cJSON_IsString(response)) {
                ESP_LOGI(TAG, "[MEDICATION] Received response: %s", response->valuestring);
                if (cJSON_IsString(transcription)) {
                    ESP_LOGI(TAG, "[MEDICATION] User said: %s", transcription->valuestring);
                }
                
                if (strcmp(response->valuestring, "yes") == 0) {
                    ESP_LOGI(TAG, "[MEDICATION] ✓ User confirmed taking medication");
                    // TODO: Store in NVS or send to cloud
                } else if (strcmp(response->valuestring, "no") == 0) {
                    ESP_LOGI(TAG, "[MEDICATION] ✗ User declined medication");
                } else {
                    ESP_LOGI(TAG, "[MEDICATION] ? Unclear response");
                }
            }
        } else if (strcmp(type->valuestring, "medication_status") == 0) {
            // Acknowledgment from server that medication status was recorded
            ESP_LOGI(TAG, "[MEDICATION] Server acknowledged medication status recording");
        } else if (strcmp(type->valuestring, "clear_medication") == 0) {
            // Server is telling us to clear medication and alarms
            ESP_LOGI(TAG, "[MEDICATION] Received clear_medication command from server");
            
            auto reason = cJSON_GetObjectItem(root, "reason");
            if (cJSON_IsString(reason)) {
                ESP_LOGI(TAG, "[MEDICATION] Clear reason: %s", reason->valuestring);
            }
            
            // Clear medication and alarms in main thread
            Schedule([this]() {
                ClearMedicationAndAlarms();
                // Send acknowledgment using the new public method
                protocol_->SendMedicationCleared();
                ESP_LOGI(TAG, "[MEDICATION] Medication and alarms cleared, acknowledgment sent");
            });
        } else if (strcmp(type->valuestring, "medication_info") == 0) {
            // Server is sending complete medication information
            ESP_LOGI(TAG, "[MEDICATION] Received medication info from server");
            
            auto med_name = cJSON_GetObjectItem(root, "medication_name");
            auto dose = cJSON_GetObjectItem(root, "dose");
            auto frequency = cJSON_GetObjectItem(root, "frequency");
            auto alarm_time = cJSON_GetObjectItem(root, "alarm_time");
            
            if (cJSON_IsString(med_name)) {
                std::string medication = med_name->valuestring;
                std::string dosage = cJSON_IsString(dose) ? dose->valuestring : "";
                std::string freq = cJSON_IsString(frequency) ? frequency->valuestring : "";
                
                // Parse alarm time if provided
                int hour = -1, minute = -1;
                if (cJSON_IsString(alarm_time)) {
                    sscanf(alarm_time->valuestring, "%d:%d", &hour, &minute);
                }
                
                Schedule([this, medication, dosage, freq, hour, minute]() {
                    // Save complete medication details
                    SaveMedicationDetails(medication, dosage, freq, hour, minute);
                    
                    ESP_LOGI(TAG, "[MEDICATION] Saved: %s %s %s at %02d:%02d", 
                             medication.c_str(), dosage.c_str(), freq.c_str(), hour, minute);
                    
                    // Show notification
                    auto display = Board::GetInstance().GetDisplay();
                    if (display) {
                        std::string msg = "Med: " + medication;
                        if (!dosage.empty()) {
                            msg += " " + dosage;
                        }
                        display->ShowNotification(msg.c_str(), 3000);
                    }
                });
            }
        } else if (strcmp(type->valuestring, "bp_reading_ack") == 0) {
            // Server acknowledges BP reading was saved
            ESP_LOGI(TAG, "[BP] Blood pressure reading acknowledged by server");
            
            auto status = cJSON_GetObjectItem(root, "status");
            auto systolic = cJSON_GetObjectItem(root, "systolic");
            auto diastolic = cJSON_GetObjectItem(root, "diastolic");
            
            if (cJSON_IsString(status) && strcmp(status->valuestring, "saved") == 0) {
                Schedule([this, systolic, diastolic]() {
                    // Show confirmation
                    auto display = Board::GetInstance().GetDisplay();
                    if (display && cJSON_IsNumber(systolic) && cJSON_IsNumber(diastolic)) {
                        char msg[64];
                        snprintf(msg, sizeof(msg), "BP %d/%d saved", 
                                (int)systolic->valuedouble, (int)diastolic->valuedouble);
                        display->ShowNotification(msg, 2000);
                    }
                    PlaySound(Lang::Sounds::P3_SUCCESS);
                });
            }
        } else if (strcmp(type->valuestring, "medication_status_ack") == 0) {
            // Server acknowledges medication status update
            ESP_LOGI(TAG, "[MEDICATION] Medication status acknowledged by server");
            
            auto status = cJSON_GetObjectItem(root, "status");
            if (cJSON_IsString(status)) {
                if (strcmp(status->valuestring, "recorded") == 0) {
                    Schedule([this]() {
                        // Mark medication as taken locally
                        MarkMedicationTaken();
                        ESP_LOGI(TAG, "[MEDICATION] Status recorded successfully");
                    });
                } else if (strcmp(status->valuestring, "failed") == 0) {
                    ESP_LOGW(TAG, "[MEDICATION] Failed to record medication status");
                }
            }
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    bool protocol_started = protocol_->Start();

    audio_debugger_ = std::make_unique<AudioDebugger>();
    audio_processor_->Initialize(codec);
    audio_processor_->OnOutput([this](std::vector<int16_t>&& data) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (audio_send_queue_.size() >= MAX_AUDIO_PACKETS_IN_QUEUE) {
                ESP_LOGW(TAG, "Too many audio packets in queue, drop the newest packet");
                return;
            }
        }
        background_task_->Schedule([this, data = std::move(data)]() mutable {
            opus_encoder_->Encode(std::move(data), [this](std::vector<uint8_t>&& opus) {
                AudioStreamPacket packet;
                packet.payload = std::move(opus);
#ifdef CONFIG_USE_SERVER_AEC
                {
                    std::lock_guard<std::mutex> lock(timestamp_mutex_);
                    if (!timestamp_queue_.empty()) {
                        packet.timestamp = timestamp_queue_.front();
                        timestamp_queue_.pop_front();
                    } else {
                        packet.timestamp = 0;
                    }

                    if (timestamp_queue_.size() > 3) { // 限制队列长度3
                        timestamp_queue_.pop_front(); // 该包发送前先出队保持队列长度
                        return;
                    }
                }
#endif
                std::lock_guard<std::mutex> lock(mutex_);
                if (audio_send_queue_.size() >= MAX_AUDIO_PACKETS_IN_QUEUE) {
                    ESP_LOGW(TAG, "Too many audio packets in queue, drop the oldest packet");
                    audio_send_queue_.pop_front();
                }
                audio_send_queue_.emplace_back(std::move(packet));
                xEventGroupSetBits(event_group_, SEND_AUDIO_EVENT);
            });
        });
    });
    audio_processor_->OnVadStateChange([this](bool speaking) {
        if (device_state_ == kDeviceStateListening) {
            Schedule([this, speaking]() {
                if (speaking) {
                    voice_detected_ = true;
                } else {
                    voice_detected_ = false;
                }
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            });
        }
    });

    wake_word_->Initialize(codec);
    wake_word_->OnWakeWordDetected([this](const std::string& wake_word) {
        Schedule([this, &wake_word]() {
            if (!protocol_) {
                return;
            }

            if (device_state_ == kDeviceStateIdle) {
                wake_word_->EncodeWakeWordData();

                if (!protocol_->IsAudioChannelOpened()) {
                    SetDeviceState(kDeviceStateConnecting);
                    if (!protocol_->OpenAudioChannel()) {
                        wake_word_->StartDetection();
                        return;
                    }
                }

                ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_USE_AFE_WAKE_WORD
                AudioStreamPacket packet;
                // Encode and send the wake word data to the server
                while (wake_word_->GetWakeWordOpus(packet.payload)) {
                    protocol_->SendAudio(packet);
                }
                // Set the chat state to wake word detected
                protocol_->SendWakeWordDetected(wake_word);
#else
                // Play the pop up sound to indicate the wake word is detected
                // And wait 60ms to make sure the queue has been processed by audio task
                ResetDecoder();
                PlaySound(Lang::Sounds::P3_POPUP);
                vTaskDelay(pdMS_TO_TICKS(60));
#endif
                SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime, false);
            } else if (device_state_ == kDeviceStateSpeaking) {
                AbortSpeaking(kAbortReasonWakeWordDetected);
            } else if (device_state_ == kDeviceStateActivating) {
                SetDeviceState(kDeviceStateIdle);
            }
        });
    });
    wake_word_->StartDetection();

    // Wait for the new version check to finish
    xEventGroupWaitBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT, pdTRUE, pdFALSE, portMAX_DELAY);
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota.HasServerTime();
    if (protocol_started) {
        std::string message = std::string(Lang::Strings::VERSION) + ota.GetCurrentVersion();
        display->ShowNotification(message.c_str());
        display->SetChatMessage("system", "");
        // Play the success sound to indicate the device is ready
        ResetDecoder();
        PlaySound(Lang::Sounds::P3_SUCCESS);
    }

    // Initialize BLE config button (moved to constructor for better initialization)

    // Load user settings from NVS (including persistent alarms)
    LoadUserSettings();

    // Print heap stats
    SystemInfo::PrintHeapStats();
    
    // Enter the main event loop
    MainEventLoop();
}

void Application::OnClockTimer() {
    clock_ticks_++;

    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar();

    // Check alarms if we have server time and no alarm is currently ringing
    if (has_server_time_ && !alarm_ringing_) {
        CheckAlarms();
    }

    // Update clock display every second if we have server time and device is idle
    if (has_server_time_ && device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            // Set status to clock "HH:MM"
            time_t now = time(NULL);
            char time_str[64];
            strftime(time_str, sizeof(time_str), "%H:%M  ", localtime(&now));
            Board::GetInstance().GetDisplay()->SetStatus(time_str);
            
            // Check for midnight reset of medication flag
            struct tm* timeinfo = localtime(&now);
            if (timeinfo->tm_hour == 0 && timeinfo->tm_min == 0 && timeinfo->tm_sec < 2) {
                // Reset at midnight (within first 2 seconds to avoid multiple resets)
                if (medication_taken_today_) {
                    ResetDailyMedication();
                    ESP_LOGI("UserSettings", "Midnight reset: cleared medication taken flag");
                }
            }
        });
    }

    // Print the debug info every 10 seconds
    if (clock_ticks_ % 10 == 0) {
        // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
        // SystemInfo::PrintTaskList();
        SystemInfo::PrintHeapStats();
    }
}

// Add a async task to MainLoop
void Application::Schedule(std::function<void()> callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, SCHEDULE_EVENT);
}

// The Main Event Loop controls the chat state and websocket connection
// If other tasks need to access the websocket or chat state,
// they should use Schedule to call this function
void Application::MainEventLoop() {
    // Raise the priority of the main event loop to avoid being interrupted by background tasks (which has priority 2)
    vTaskPrioritySet(NULL, 3);

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, SCHEDULE_EVENT | SEND_AUDIO_EVENT, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & SEND_AUDIO_EVENT) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto packets = std::move(audio_send_queue_);
            lock.unlock();
            for (auto& packet : packets) {
                if (!protocol_->SendAudio(packet)) {
                    break;
                }
            }
        }

        if (bits & SCHEDULE_EVENT) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }
    }
}

// The Audio Loop is used to input and output audio data
void Application::AudioLoop() {
    auto codec = Board::GetInstance().GetAudioCodec();
    while (true) {
        OnAudioInput();
        if (codec->output_enabled()) {
            OnAudioOutput();
        }
    }
}

void Application::OnAudioOutput() {
    if (busy_decoding_audio_) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    auto codec = Board::GetInstance().GetAudioCodec();
    const int max_silence_seconds = 10;

    std::unique_lock<std::mutex> lock(mutex_);
    if (audio_decode_queue_.empty()) {
        // Disable the output if there is no audio data for a long time
        if (device_state_ == kDeviceStateIdle) {
            auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_output_time_).count();
            if (duration > max_silence_seconds) {
                codec->EnableOutput(false);
            }
        }
        return;
    }

    auto packet = std::move(audio_decode_queue_.front());
    audio_decode_queue_.pop_front();
    lock.unlock();
    audio_decode_cv_.notify_all();

    // Synchronize the sample rate and frame duration
    SetDecodeSampleRate(packet.sample_rate, packet.frame_duration);

    busy_decoding_audio_ = true;
    if (!background_task_->Schedule([this, codec, packet = std::move(packet)]() mutable {
        busy_decoding_audio_ = false;
        if (aborted_) {
            return;
        }

        std::vector<int16_t> pcm;
        if (!opus_decoder_->Decode(std::move(packet.payload), pcm)) {
            return;
        }
        // Resample if the sample rate is different
        if (opus_decoder_->sample_rate() != codec->output_sample_rate()) {
            int target_size = output_resampler_.GetOutputSamples(pcm.size());
            std::vector<int16_t> resampled(target_size);
            output_resampler_.Process(pcm.data(), pcm.size(), resampled.data());
            pcm = std::move(resampled);
        }
        codec->OutputData(pcm);
#ifdef CONFIG_USE_SERVER_AEC
        std::lock_guard<std::mutex> lock(timestamp_mutex_);
        timestamp_queue_.push_back(packet.timestamp);
#endif
        last_output_time_ = std::chrono::steady_clock::now();
    })) {
        busy_decoding_audio_ = false;
    }
}

void Application::OnAudioInput() {
    if (device_state_ == kDeviceStateAudioTesting) {
        if (audio_testing_queue_.size() >= AUDIO_TESTING_MAX_DURATION_MS / OPUS_FRAME_DURATION_MS) {
            ExitAudioTestingMode();
            return;
        }
        std::vector<int16_t> data;
        int samples = OPUS_FRAME_DURATION_MS * 16000 / 1000;
        if (ReadAudio(data, 16000, samples)) {
            background_task_->Schedule([this, data = std::move(data)]() mutable {
                opus_encoder_->Encode(std::move(data), [this](std::vector<uint8_t>&& opus) {
                    AudioStreamPacket packet;
                    packet.payload = std::move(opus);
                    packet.frame_duration = OPUS_FRAME_DURATION_MS;
                    packet.sample_rate = 16000;
                    std::lock_guard<std::mutex> lock(mutex_);
                    audio_testing_queue_.push_back(std::move(packet));
                });
            });
            return;
        }
    }

    // Enable wake word detection during playback for barge-in
    if (wake_word_->IsDetectionRunning()) {
        std::vector<int16_t> data;
        int samples = wake_word_->GetFeedSize();
        if (samples > 0) {
            if (ReadAudio(data, 16000, samples)) {
                wake_word_->Feed(data);
                // Barge-in is handled by the wake word callback
                // which calls AbortSpeaking(kAbortReasonWakeWordDetected) if speaking
                return;
            }
        }
    }

    if (audio_processor_->IsRunning()) {
        std::vector<int16_t> data;
        int samples = audio_processor_->GetFeedSize();
        if (samples > 0) {
            if (ReadAudio(data, 16000, samples)) {
                // Log only occasionally to avoid flooding logs
                static int feed_counter = 0;
                if (medication_context_ && feed_counter++ % 50 == 0) {
                    ESP_LOGI(TAG, "[AUDIO] Feeding %d samples to processor (medication context active)", samples);
                }
                audio_processor_->Feed(data);
                return;
            } else if (medication_context_) {
                static int read_fail_counter = 0;
                if (read_fail_counter++ % 50 == 0) {
                    ESP_LOGW(TAG, "[AUDIO] Failed to read audio data (medication context active)");
                }
            }
        }
    }

    vTaskDelay(pdMS_TO_TICKS(OPUS_FRAME_DURATION_MS / 2));
}

bool Application::ReadAudio(std::vector<int16_t>& data, int sample_rate, int samples) {
    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec->input_enabled()) {
        return false;
    }

    if (codec->input_sample_rate() != sample_rate) {
        data.resize(samples * codec->input_sample_rate() / sample_rate);
        if (!codec->InputData(data)) {
            return false;
        }
        if (codec->input_channels() == 2) {
            auto mic_channel = std::vector<int16_t>(data.size() / 2);
            auto reference_channel = std::vector<int16_t>(data.size() / 2);
            for (size_t i = 0, j = 0; i < mic_channel.size(); ++i, j += 2) {
                mic_channel[i] = data[j];
                reference_channel[i] = data[j + 1];
            }
            auto resampled_mic = std::vector<int16_t>(input_resampler_.GetOutputSamples(mic_channel.size()));
            auto resampled_reference = std::vector<int16_t>(reference_resampler_.GetOutputSamples(reference_channel.size()));
            input_resampler_.Process(mic_channel.data(), mic_channel.size(), resampled_mic.data());
            reference_resampler_.Process(reference_channel.data(), reference_channel.size(), resampled_reference.data());
            data.resize(resampled_mic.size() + resampled_reference.size());
            for (size_t i = 0, j = 0; i < resampled_mic.size(); ++i, j += 2) {
                data[j] = resampled_mic[i];
                data[j + 1] = resampled_reference[i];
            }
        } else {
            auto resampled = std::vector<int16_t>(input_resampler_.GetOutputSamples(data.size()));
            input_resampler_.Process(data.data(), data.size(), resampled.data());
            data = std::move(resampled);
        }
    } else {
        data.resize(samples);
        if (!codec->InputData(data)) {
            return false;
        }
    }
    
    // 音频调试：发送原始音频数据
    if (audio_debugger_) {
        audio_debugger_->Feed(data);
    }
    
    return true;
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    protocol_->SendAbortSpeaking(reason);
}

void Application::SetListeningMode(ListeningMode mode, bool medication_context) {
    listening_mode_ = mode;
    medication_context_ = medication_context;
    
    // Force audio processor restart if medication context to ensure clean audio capture
    if (medication_context_ && audio_processor_->IsRunning()) {
        ESP_LOGI(TAG, "[MEDICATION] Forcing audio processor restart for clean capture");
        audio_processor_->Stop();
    }
    
    SetDeviceState(kDeviceStateListening);
}

void Application::SetDeviceState(DeviceState state) {
    if (device_state_ == state) {
        return;
    }
    
    clock_ticks_ = 0;
    auto previous_state = device_state_;
    device_state_ = state;
    ESP_LOGI(TAG, "STATE: %s", STATE_STRINGS[device_state_]);
    // The state is changed, wait for all background tasks to finish
    background_task_->WaitForCompletion();

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    switch (state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
            
            // Both AEC and VAD are always active - no switching needed
            
            audio_processor_->Stop();
            wake_word_->StartDetection();
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            timestamp_queue_.clear();
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");
            // Update the IoT states before sending the start listening command
#if CONFIG_IOT_PROTOCOL_XIAOZHI
            UpdateIotStates();
#endif

            // Make sure the audio processor is running
            ESP_LOGI(TAG, "[LISTENING] Checking audio processor state, is_running=%d, medication_context=%d", 
                     audio_processor_->IsRunning(), medication_context_);
            
            // Send the start listening command with medication context if set
            ESP_LOGI(TAG, "[LISTENING] Sending start listening command to server with medication_context=%d", medication_context_);
            protocol_->SendStartListening(listening_mode_, medication_context_);
            medication_context_ = false;  // Reset after sending
            
            // Both AEC and VAD are always active - no switching needed
            ESP_LOGI(TAG, "[LISTENING] AEC and VAD both active for optimal performance");
            
            if (!audio_processor_->IsRunning()) {
                ESP_LOGI(TAG, "[LISTENING] Starting audio processor for listening");
                if (previous_state == kDeviceStateSpeaking) {
                    ESP_LOGI(TAG, "[LISTENING] Previous state was speaking, waiting for audio to finish");
                    // DON'T clear the audio queue - let remaining audio play out
                    // The audio playback thread will continue processing the queue
                    // audio_decode_queue_.clear();  // REMOVED - this was deleting unplayed audio!
                    audio_decode_cv_.notify_all();
                    // TODO: Consider waiting for the queue to empty before fully transitioning
                    vTaskDelay(pdMS_TO_TICKS(120));
                }
                ESP_LOGI(TAG, "[LISTENING] Resetting opus encoder");
                opus_encoder_->ResetState();
                ESP_LOGI(TAG, "[LISTENING] Starting audio processor");
                audio_processor_->Start();
                ESP_LOGI(TAG, "[LISTENING] Stopping wake word detection");
                wake_word_->StopDetection();
                ESP_LOGI(TAG, "[LISTENING] Audio processor started successfully");
            } else {
                ESP_LOGI(TAG, "[LISTENING] Audio processor already running");
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_processor_->Stop();
                
                // Both AEC and VAD are always active - perfect for barge-in
                ESP_LOGI(TAG, "AEC and VAD both active - barge-in ready");
                
                // Always enable wake word for barge-in with dual mic + AEC
                ESP_LOGI(TAG, "Enabling wake word detection for barge-in");
                wake_word_->StartDetection();
            }
            ResetDecoder();
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::ResetDecoder() {
    std::lock_guard<std::mutex> lock(mutex_);
    opus_decoder_->ResetState();
    audio_decode_queue_.clear();
    audio_decode_cv_.notify_all();
    last_output_time_ = std::chrono::steady_clock::now();
    auto codec = Board::GetInstance().GetAudioCodec();
    codec->EnableOutput(true);
}

void Application::SetDecodeSampleRate(int sample_rate, int frame_duration) {
    if (opus_decoder_->sample_rate() == sample_rate && opus_decoder_->duration_ms() == frame_duration) {
        return;
    }

    opus_decoder_.reset();
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(sample_rate, 1, frame_duration);

    auto codec = Board::GetInstance().GetAudioCodec();
    if (opus_decoder_->sample_rate() != codec->output_sample_rate()) {
        ESP_LOGI(TAG, "Resampling audio from %d to %d", opus_decoder_->sample_rate(), codec->output_sample_rate());
        output_resampler_.Configure(opus_decoder_->sample_rate(), codec->output_sample_rate());
    }
}

void Application::UpdateIotStates() {
#if CONFIG_IOT_PROTOCOL_XIAOZHI
    auto& thing_manager = iot::ThingManager::GetInstance();
    std::string states;
    if (thing_manager.GetStatesJson(states, true)) {
        protocol_->SendIotStates(states);
    }
#endif
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    esp_restart();
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (device_state_ == kDeviceStateIdle) {
        ToggleChatState();
        Schedule([this, wake_word]() {
            if (protocol_) {
                protocol_->SendWakeWordDetected(wake_word); 
            }
        }); 
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (device_state_ != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    Schedule([this, payload]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_processor_->EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_processor_->EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_processor_->EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

// Alarm clock implementation
void Application::CheckAlarms() {
    time_t now = time(nullptr);
    struct tm *timeinfo = localtime(&now);
    
    for (auto& alarm : alarms_) {
        if (alarm.enabled && !alarm.triggered) {
            if (timeinfo->tm_hour == alarm.hour && 
                timeinfo->tm_min == alarm.minute) {
                ESP_LOGI("Alarm", "Alarm time reached: %02d:%02d", alarm.hour, alarm.minute);
                alarm.triggered = true;
                TriggerAlarm();
                break;
            }
        }
    }
    
    // Reset triggered flags at midnight
    if (timeinfo->tm_hour == 0 && timeinfo->tm_min == 0 && timeinfo->tm_sec == 0) {
        for (auto& alarm : alarms_) {
            alarm.triggered = false;
        }
        ESP_LOGI("Alarm", "Reset alarm triggered flags at midnight");
    }
}

void Application::TriggerAlarm() {
    ESP_LOGI("Alarm", "Alarm triggered!");
    
    // Mark alarm as sounded for today
    MarkAlarmSounded();
    
    // Schedule alarm sound and notification in main thread
    Schedule([this]() {
        // Use SUCCESS sound - same loud sound played when button is pressed
        Alert("ALARM!", "Time's up!", "surprised", Lang::Sounds::P3_SUCCESS);
        
        // If this is a medication reminder, send notification to server
        if (!medication_name_.empty() && protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->SendReminderSent("medication_time");
            ESP_LOGI("Alarm", "Sent medication reminder notification to server");
        }
        
        // Clear the alert after 3 seconds
        background_task_->Schedule([this]() {
            vTaskDelay(pdMS_TO_TICKS(3000));
            Schedule([this]() {
                DismissAlert();
            });
        });
    });
    
    // Mark as not ringing since it's just a single notification
    alarm_ringing_ = false;
}

void Application::StopAlarm() {
    if (alarm_ringing_) {
        alarm_ringing_ = false;
        ESP_LOGI("Alarm", "Alarm stopped");
        
        // Clear alarm notification
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->ShowNotification("");
            display->SetEmotion("neutral");
        });
        
        // Play confirmation sound
        PlaySound(Lang::Sounds::P3_SUCCESS);
    }
}

bool Application::SetAlarm(uint8_t hour, uint8_t minute) {
    // Validate input
    if (hour > 23 || minute > 59) {
        ESP_LOGE("Alarm", "Invalid alarm time: %02d:%02d", hour, minute);
        return false;
    }
    
    // Check if alarm already exists
    for (auto& alarm : alarms_) {
        if (alarm.hour == hour && alarm.minute == minute) {
            alarm.enabled = true;
            alarm.triggered = false;
            ESP_LOGI("Alarm", "Updated existing alarm at %02d:%02d", hour, minute);
            return true;
        }
    }
    
    // Add new alarm
    Alarm new_alarm;
    new_alarm.hour = hour;
    new_alarm.minute = minute;
    new_alarm.enabled = true;
    new_alarm.triggered = false;
    alarms_.push_back(new_alarm);
    
    ESP_LOGI("Alarm", "Set new alarm at %02d:%02d", hour, minute);
    
    // Save to NVS for persistence across reboots
    SaveAlarmToNVS(hour, minute);
    
    // Show confirmation and play sound in main thread
    Schedule([this, hour, minute]() {
        char msg[32];
        snprintf(msg, sizeof(msg), "Alarm set: %02d:%02d", hour, minute);
        auto display = Board::GetInstance().GetDisplay();
        display->ShowNotification(msg, 2000);  // 2 second duration
        
        // Play confirmation sound
        ResetDecoder();
        PlaySound(Lang::Sounds::P3_SUCCESS);
    });
    
    return true;
}

void Application::ClearAllAlarms() {
    alarms_.clear();
    alarm_ringing_ = false;
    ESP_LOGI("Alarm", "All alarms cleared");
    
    // Clear from NVS
    ClearAlarmsFromNVS();
    
    // Show confirmation and play sound in main thread
    Schedule([this]() {
        auto display = Board::GetInstance().GetDisplay();
        display->ShowNotification("Alarms cleared", 2000);  // 2 second duration
        
        // Play confirmation sound
        ResetDecoder();
        PlaySound(Lang::Sounds::P3_SUCCESS);
    });
}

// User Settings Implementation
void Application::LoadUserSettings() {
    Settings user_settings("user", false);  // Read-only access
    
    // Load user ID
    user_id_ = user_settings.GetString("user_id", "");
    if (!user_id_.empty()) {
        ESP_LOGI("UserSettings", "Loaded user_id: %s", user_id_.c_str());
    }
    
    // Load alarm settings
    int alarm_hour = user_settings.GetInt("alarm_hour", -1);
    int alarm_minute = user_settings.GetInt("alarm_minute", -1);
    if (alarm_hour >= 0 && alarm_hour <= 23 && 
        alarm_minute >= 0 && alarm_minute <= 59) {
        SetAlarm(alarm_hour, alarm_minute);
        ESP_LOGI("UserSettings", "Restored alarm: %02d:%02d", alarm_hour, alarm_minute);
    }
    
    // Load medication info
    medication_name_ = user_settings.GetString("medication", "");
    medication_taken_today_ = user_settings.GetInt("taken_today", 0) == 1;
    alarm_sounded_today_ = user_settings.GetInt("alarm_sounded", 0) == 1;
    if (!medication_name_.empty()) {
        ESP_LOGI("UserSettings", "Loaded medication: %s, taken today: %s, alarm sounded: %s", 
                 medication_name_.c_str(), medication_taken_today_ ? "yes" : "no",
                 alarm_sounded_today_ ? "yes" : "no");
    }
    
    // Load enhanced medication details
    medication_dosage_ = user_settings.GetString("med_dosage", "");
    medication_frequency_ = user_settings.GetString("med_frequency", "");
    if (!medication_dosage_.empty()) {
        ESP_LOGI("UserSettings", "Loaded medication dosage: %s, frequency: %s", 
                 medication_dosage_.c_str(), medication_frequency_.c_str());
    }
    
    // Load medication reminder time
    med_reminder_hour_ = user_settings.GetInt("med_alarm_hour", -1);
    med_reminder_minute_ = user_settings.GetInt("med_alarm_min", -1);
    if (med_reminder_hour_ >= 0 && med_reminder_minute_ >= 0) {
        ESP_LOGI("UserSettings", "Medication reminder set for %02d:%02d", 
                 med_reminder_hour_, med_reminder_minute_);
    }
    
    // Load blood pressure data
    bp_systolic_ = user_settings.GetInt("bp_systolic", 0);
    bp_diastolic_ = user_settings.GetInt("bp_diastolic", 0);
    bp_pulse_ = user_settings.GetInt("bp_pulse", 0);
    bp_timestamp_ = static_cast<time_t>(user_settings.GetInt("bp_timestamp", 0));
    if (bp_systolic_ > 0 && bp_diastolic_ > 0) {
        ESP_LOGI("UserSettings", "Loaded BP: %d/%d, pulse: %d", 
                 bp_systolic_, bp_diastolic_, bp_pulse_);
    }
    
    // Load sync time
    last_sync_time_ = static_cast<time_t>(user_settings.GetInt("last_sync_time", 0));
}

void Application::SaveAlarmToNVS(uint8_t hour, uint8_t minute) {
    Settings user_settings("user", true);  // Read-write access
    user_settings.SetInt("alarm_hour", hour);
    user_settings.SetInt("alarm_minute", minute);
    ESP_LOGI("UserSettings", "Saved alarm to NVS: %02d:%02d", hour, minute);
}

void Application::ClearAlarmsFromNVS() {
    Settings user_settings("user", true);
    user_settings.SetInt("alarm_hour", -1);
    user_settings.SetInt("alarm_minute", -1);
    ESP_LOGI("UserSettings", "Cleared alarms from NVS");
}

void Application::SaveMedicationInfo(const std::string& medication) {
    Settings user_settings("user", true);
    user_settings.SetString("medication", medication);
    medication_name_ = medication;
    ESP_LOGI("UserSettings", "Saved medication to NVS: %s", medication.c_str());
}

void Application::MarkMedicationTaken() {
    Settings user_settings("user", true);
    user_settings.SetInt("taken_today", 1);
    medication_taken_today_ = true;
    ESP_LOGI("UserSettings", "Marked medication as taken for today");
    
    // Show confirmation
    Schedule([this]() {
        auto display = Board::GetInstance().GetDisplay();
        display->ShowNotification("Medication recorded", 2000);
        PlaySound(Lang::Sounds::P3_SUCCESS);
    });
}

void Application::ResetDailyMedication() {
    Settings user_settings("user", true);
    user_settings.SetInt("taken_today", 0);
    user_settings.SetInt("alarm_sounded", 0);  // Also reset alarm sounded flag
    medication_taken_today_ = false;
    alarm_sounded_today_ = false;
    ESP_LOGI("UserSettings", "Reset daily medication and alarm flags");
}

void Application::SaveBloodPressure(int systolic, int diastolic, int pulse) {
    bp_systolic_ = systolic;
    bp_diastolic_ = diastolic;
    bp_pulse_ = pulse;
    bp_timestamp_ = time(nullptr);
    
    Settings user_settings("user", true);
    user_settings.SetInt("bp_systolic", systolic);
    user_settings.SetInt("bp_diastolic", diastolic);
    user_settings.SetInt("bp_pulse", pulse);
    user_settings.SetInt("bp_timestamp", static_cast<int>(bp_timestamp_));
    
    ESP_LOGI("UserSettings", "Blood pressure saved: %d/%d, pulse: %d", systolic, diastolic, pulse);
    
    // Show confirmation
    Schedule([this, systolic, diastolic]() {
        auto display = Board::GetInstance().GetDisplay();
        char msg[64];
        snprintf(msg, sizeof(msg), "BP: %d/%d saved", systolic, diastolic);
        display->ShowNotification(msg, 2000);
        PlaySound(Lang::Sounds::P3_SUCCESS);
    });
}

void Application::GetBloodPressure(int& systolic, int& diastolic, int& pulse) const {
    systolic = bp_systolic_;
    diastolic = bp_diastolic_;
    pulse = bp_pulse_;
}

void Application::SaveMedicationDetails(const std::string& name, const std::string& dosage, 
                                       const std::string& frequency, int hour, int minute) {
    medication_name_ = name;
    medication_dosage_ = dosage;
    medication_frequency_ = frequency;
    med_reminder_hour_ = hour;
    med_reminder_minute_ = minute;
    
    Settings user_settings("user", true);
    user_settings.SetString("medication", name);
    user_settings.SetString("med_dosage", dosage);
    user_settings.SetString("med_frequency", frequency);
    user_settings.SetInt("med_alarm_hour", hour);
    user_settings.SetInt("med_alarm_min", minute);
    
    // Also set the alarm if valid time provided
    if (hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59) {
        SetAlarm(hour, minute);
    }
    
    ESP_LOGI("UserSettings", "Saved medication details: %s %s %s at %02d:%02d", 
             name.c_str(), dosage.c_str(), frequency.c_str(), hour, minute);
}

void Application::MarkAlarmSounded() {
    alarm_sounded_today_ = true;
    Settings user_settings("user", true);
    user_settings.SetInt("alarm_sounded", 1);
    ESP_LOGI("UserSettings", "Marked alarm as sounded for today");
}

void Application::ClearMedicationAndAlarms() {
    ESP_LOGI(TAG, "Clearing medication and alarms from NVS");
    
    Settings user_settings("user", true);  // Read-write access
    
    // Clear medication-related keys
    user_settings.EraseKey("medication");
    user_settings.EraseKey("taken_today");
    user_settings.EraseKey("med_alarm_hour");
    user_settings.EraseKey("med_alarm_min");
    user_settings.EraseKey("med_dosage");
    user_settings.EraseKey("med_frequency");
    user_settings.EraseKey("alarm_sounded");
    
    // Clear local variables
    medication_name_.clear();
    medication_taken_today_ = false;
    med_reminder_hour_ = -1;
    med_reminder_minute_ = -1;
    medication_dosage_.clear();
    medication_frequency_.clear();
    alarm_sounded_today_ = false;
    
    // Show notification to user
    auto display = Board::GetInstance().GetDisplay();
    if (display) {
        display->ShowNotification("Medication cleared", 3000);
    }
    
    ESP_LOGI(TAG, "Medication and medication alarms cleared successfully");
}

void Application::StartBleConfigMode() {
    ESP_LOGI(TAG, "StartBleConfigMode called from button press");
    
    // Get the board instance
    Board& board = Board::GetInstance();
    
    // Log the board type for debugging
    std::string board_type = board.GetBoardType();
    ESP_LOGI(TAG, "Board type detected: %s", board_type.c_str());
    
    // Check if it's a WiFi board by checking the board type
    if (board_type == "wifi") {
        ESP_LOGI(TAG, "WiFi board confirmed, casting to WifiBoard");
        // Cast to WifiBoard (safe because we checked the type)
        WifiBoard* wifi_board = static_cast<WifiBoard*>(&board);
        ESP_LOGI(TAG, "Calling EnterBleConfigMode on WifiBoard");
        // Call the existing EnterBleConfigMode function
        wifi_board->EnterBleConfigMode();
        ESP_LOGI(TAG, "EnterBleConfigMode call completed");
    } else {
        ESP_LOGE(TAG, "Board does not support BLE configuration - board type is: %s", board_type.c_str());
    }
}

#ifdef CONFIG_USE_EYE_ANIMATION_STYLE

void Application::InitializeEyeAnimation() {
    ESP_LOGI(TAG, "Initializing eye animation system");
    
    // Set default eye style
    SetEyeStyle(7);  // Default style
    
    // Initialize blink timing
    timeOfLastBlink = esp_timer_get_time();
    timeToNextBlink = 3000000 + (esp_random() % 4000000);  // 3-7 seconds
    
    // Create eye animation task
    xTaskCreate([](void* param) {
        Application* app = static_cast<Application*>(param);
        app->EyeAnimationLoop();
    }, "eye_animation", 4096, this, 5, &eye_animation_task_handle_);
    
    ESP_LOGI(TAG, "Eye animation system initialized");
}

void Application::EyeAnimationLoop() {
    ESP_LOGI(TAG, "Eye animation loop started");
    
    auto display = Board::GetInstance().GetDisplay();
    if (!display) {
        ESP_LOGE(TAG, "No display available for eye animation");
        vTaskDelete(nullptr);
        return;
    }
    
    ESP_LOGI(TAG, "Eye animation display available, starting animation");
    int frame_count = 0;
    
    // Create a simple smiley face emoticon for 128x32 display (RGB565 format)
    // This is a 32x32 emoticon that will fit on the display
    uint16_t smiley_face[32*32];
    
    // Fill with black background
    for (int i = 0; i < 32*32; i++) {
        smiley_face[i] = 0x0000; // Black in RGB565
    }
    
    // Draw a simple smiley face (white pixels = 0xFFFF in RGB565)
    // Draw circle outline (simplified)
    for (int x = 8; x < 24; x++) {
        smiley_face[4*32 + x] = 0xFFFF;   // Top
        smiley_face[27*32 + x] = 0xFFFF;  // Bottom
    }
    for (int y = 8; y < 24; y++) {
        smiley_face[y*32 + 4] = 0xFFFF;   // Left
        smiley_face[y*32 + 27] = 0xFFFF;  // Right
    }
    
    // Draw eyes (two dots)
    smiley_face[12*32 + 10] = 0xFFFF;
    smiley_face[12*32 + 11] = 0xFFFF;
    smiley_face[13*32 + 10] = 0xFFFF;
    smiley_face[13*32 + 11] = 0xFFFF;
    
    smiley_face[12*32 + 20] = 0xFFFF;
    smiley_face[12*32 + 21] = 0xFFFF;
    smiley_face[13*32 + 20] = 0xFFFF;
    smiley_face[13*32 + 21] = 0xFFFF;
    
    // Draw smile (arc)
    for (int x = 10; x < 22; x++) {
        smiley_face[20*32 + x] = 0xFFFF;
    }
    smiley_face[19*32 + 9] = 0xFFFF;
    smiley_face[19*32 + 22] = 0xFFFF;
    
    ESP_LOGI(TAG, "Created smiley face emoticon, starting display loop");
    
    // Simple animation loop
    while (true) {
        uint64_t currentTime = esp_timer_get_time();
        
        // Log every 100 frames
        if (frame_count % 100 == 0) {
            ESP_LOGI(TAG, "Drawing emoticon frame %d", frame_count);
        }
        
        // Display the smiley face emoticon
        // Position it in different spots to create animation
        int x_offset = (frame_count / 10) % 96;  // Move horizontally
        display->SetEye(x_offset, 0, x_offset + 32, 32, smiley_face);
        
        frame_count++;
        vTaskDelay(pdMS_TO_TICKS(100));  // 10 FPS for simple animation
    }
}

void Application::SetEyeStyle(uint8_t style) {
    ESP_LOGI(TAG, "Setting eye style to %d", style);
    eye_style_num = style;
    
    // Set eye graphics based on style
    // For now, just use default style
    iris = iris_default;
    sclera = sclera_default;
    
    // In the future, you can add more styles like RoPet:
    // case 2: iris = iris_style_blood; sclera = sclera_style_white; break;
    // case 3: iris = iris_style_cospa1; sclera = sclera_style_cute_girl; break;
    // etc.
}

void Application::UpdateEyeExpression(const std::string& emotion) {
    ESP_LOGI(TAG, "Updating eye expression for emotion: %s", emotion.c_str());
    
    // Map emotions to eye styles
    if (emotion == "happy") {
        SetEyeStyle(1);
    } else if (emotion == "sad") {
        SetEyeStyle(2);
    } else if (emotion == "angry") {
        SetEyeStyle(3);
    } else if (emotion == "surprised") {
        SetEyeStyle(4);
    } else {
        SetEyeStyle(7);  // Default
    }
}

#endif // CONFIG_USE_EYE_ANIMATION_STYLE
