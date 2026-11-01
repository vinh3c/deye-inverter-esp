#include "logger_config.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <esp_log.h>
#include <esp_matter_console.h>
#include <nvs.h>

namespace logger_config {
namespace {
constexpr char TAG[] = "logger_config";
constexpr char NAMESPACE[] = "deye_logger";
struct Settings {
    uint32_t version;
    char host[64];
    uint16_t port;
};

bool valid_host(const char *host)
{
    const size_t length = std::strlen(host);
    if (!length || length > 63 || !std::isalnum(static_cast<unsigned char>(host[0])) ||
        !std::isalnum(static_cast<unsigned char>(host[length - 1]))) return false;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = host[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '.')) return false;
    }
    return true;
}

esp_err_t load(Settings &settings)
{
    nvs_handle_t handle;
    auto error = nvs_open(NAMESPACE, NVS_READONLY, &handle);
    if (error != ESP_OK) return error;
    size_t size = sizeof(settings);
    error = nvs_get_blob(handle, "settings", &settings, &size);
    nvs_close(handle);
    if (error != ESP_OK) return error;
    if (size != sizeof(settings) || settings.version != 1 ||
        !std::memchr(settings.host, '\0', sizeof(settings.host)) ||
        !valid_host(settings.host) || !settings.port) return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

esp_err_t command(int argc, char **argv)
{
    if (argc == 0) {
        Settings settings{};
        const auto error = load(settings);
        if (error == ESP_OK) {
            std::printf("LOGGER_CONFIG host=%s port=%u (stored; simulator still active)\n", settings.host, settings.port);
        } else {
            std::printf("LOGGER_CONFIG not configured: %s\n", esp_err_to_name(error));
        }
        return error == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : error;
    }
    // The request token lets the host distinguish a fresh commit acknowledgement
    // from old log output or the shell's echoed input.
    if (argc != 4 || std::strcmp(argv[0], "set") || !valid_host(argv[1]) || std::strlen(argv[3]) != 8)
        return ESP_ERR_INVALID_ARG;
    for (const char *c = argv[2]; *c; ++c) if (*c < '0' || *c > '9') return ESP_ERR_INVALID_ARG;
    for (const char *c = argv[3]; *c; ++c) if (!std::isxdigit(static_cast<unsigned char>(*c))) return ESP_ERR_INVALID_ARG;
    char *end;
    const auto port = std::strtoul(argv[2], &end, 10);
    if (!*argv[2] || *end || port == 0 || port > 65535) return ESP_ERR_INVALID_ARG;
    Settings settings{};
    settings.version = 1;
    std::strcpy(settings.host, argv[1]);
    settings.port = static_cast<uint16_t>(port);
    nvs_handle_t handle;
    auto error = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    error = nvs_set_blob(handle, "settings", &settings, sizeof(settings));
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    if (error != ESP_OK) return error;
    Settings stored{};
    error = load(stored);
    if (error != ESP_OK) return error;
    if (std::strcmp(stored.host, settings.host) || stored.port != settings.port) return ESP_FAIL;
    std::printf("LOGGER_CONFIG_SAVED token=%s host=%s port=%u\n", argv[3], stored.host, stored.port);
    std::fflush(stdout);
    return ESP_OK;
}
} // namespace

void init()
{
    Settings settings{};
    const auto error = load(settings);
    if (error == ESP_OK) {
        ESP_LOGI(TAG, "Logger host=%s port=%u (stored; simulator still active)", settings.host, settings.port);
    } else if (error == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "Logger not configured; simulator still active");
    } else {
        ESP_LOGW(TAG, "Cannot load logger config: %s; simulator still active", esp_err_to_name(error));
    }
}

void register_console()
{
    static const esp_matter::console::command_t commands[] = {
        {"logger", "Show config, or: matter esp logger set <host> <port> <8-hex-token>", command},
    };
    ESP_ERROR_CHECK(esp_matter::console::add_commands(commands, 1));
}
} // namespace logger_config
