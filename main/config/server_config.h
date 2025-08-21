#ifndef SERVER_CONFIG_H
#define SERVER_CONFIG_H

#include <string>

// These values are set by CMake from .env.secret
#ifndef CONFIG_WEBSOCKET_URL
#error "CONFIG_WEBSOCKET_URL not defined! Create .env.secret from .env.secret.example"
#endif

#ifndef CONFIG_OTA_URL
#error "CONFIG_OTA_URL not defined! Create .env.secret from .env.secret.example"
#endif

class ServerConfig {
public:
    static ServerConfig& GetInstance() {
        static ServerConfig instance;
        return instance;
    }

    const std::string& GetWebSocketUrl() const {
        return websocket_url_;
    }

    const std::string& GetOtaUrl() const {
        return ota_url_;
    }

private:
    ServerConfig() {
        // Load from build-time configuration
        websocket_url_ = CONFIG_WEBSOCKET_URL;
        ota_url_ = CONFIG_OTA_URL;
    }

    std::string websocket_url_;
    std::string ota_url_;
};

#endif // SERVER_CONFIG_H