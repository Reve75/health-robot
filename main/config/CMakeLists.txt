# Configuration component for server settings

# Check if .env.secret exists first (production values)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../.env.secret")
    message(STATUS "Loading server configuration from .env.secret")
    
    # Read the secret configuration file
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../../.env.secret" CONFIG_LINES)
    
    foreach(LINE ${CONFIG_LINES})
        # Skip comments and empty lines
        if(NOT LINE MATCHES "^#" AND NOT LINE STREQUAL "")
            # Parse KEY=VALUE format
            if(LINE MATCHES "^([^=]+)=(.*)$")
                set(CONFIG_KEY ${CMAKE_MATCH_1})
                set(CONFIG_VALUE ${CMAKE_MATCH_2})
                
                # Remove quotes if present
                string(REGEX REPLACE "^\"(.*)\"$" "\\1" CONFIG_VALUE "${CONFIG_VALUE}")
                string(REGEX REPLACE "^'(.*)'$" "\\1" CONFIG_VALUE "${CONFIG_VALUE}")
                
                # Set compile definitions based on config keys
                if(CONFIG_KEY STREQUAL "WEBSOCKET_URL")
                    add_compile_definitions(CONFIG_WEBSOCKET_URL="${CONFIG_VALUE}")
                    message(STATUS "  WebSocket URL: ${CONFIG_VALUE}")
                    
                elseif(CONFIG_KEY STREQUAL "OTA_URL")
                    add_compile_definitions(CONFIG_OTA_URL="${CONFIG_VALUE}")
                    message(STATUS "  OTA URL: ${CONFIG_VALUE}")
                endif()
            endif()
        endif()
    endforeach()
else()
    message(FATAL_ERROR ".env.secret not found! Copy .env.secret.example to .env.secret and configure your server URLs")
endif()

# Also define CONFIG_OTA_URL for backward compatibility with ota.cc
if(DEFINED CONFIG_OTA_URL)
    add_compile_definitions(CONFIG_OTA_URL="${CONFIG_OTA_URL}")
endif()

# Add the config directory to include paths
idf_component_register(
    INCLUDE_DIRS "."
)