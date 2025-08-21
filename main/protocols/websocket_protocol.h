#ifndef _WEBSOCKET_PROTOCOL_H_
#define _WEBSOCKET_PROTOCOL_H_


#include "protocol.h"

#include <web_socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#define WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

class WebsocketProtocol : public Protocol {
public:
    WebsocketProtocol();
    ~WebsocketProtocol();

    bool Start() override;
    bool SendAudio(const AudioStreamPacket& packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel() override;
    bool IsAudioChannelOpened() const override;

private:
    EventGroupHandle_t event_group_handle_;
    WebSocket* websocket_ = nullptr;
    WebSocket* persistent_websocket_ = nullptr;  // Persistent connection for control messages
    int version_ = 1;
    
    // Heartbeat and reconnection
    TaskHandle_t heartbeat_task_ = nullptr;
    TaskHandle_t reconnect_task_ = nullptr;
    bool persistent_connected_ = false;
    uint32_t last_heartbeat_time_ = 0;
    uint32_t reconnect_attempts_ = 0;
    

    void ParseServerHello(const cJSON* root);
    bool SendText(const std::string& text) override;
    std::string GetHelloMessage();
    
    // Persistent connection methods
    bool StartPersistentConnection();
    void StopPersistentConnection();
    bool SendHeartbeat();
    static void HeartbeatTask(void* param);
    static void ReconnectTask(void* param);
    void HandlePersistentMessage(const char* data, size_t len);
};

#endif
