#pragma once
#include <cstdint>
#include <esp_err.h>

namespace logger_config {
struct Settings {
    uint32_t version;
    char host[64];
    uint16_t port;
};
esp_err_t get(Settings &settings);
void init();
void register_console();
} // namespace logger_config
