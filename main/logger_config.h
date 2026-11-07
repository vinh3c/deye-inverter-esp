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
// Commits and reads back the same blob used by console and web configuration.
esp_err_t set(const char *host, uint16_t port);
void init();
void register_console();
} // namespace logger_config
