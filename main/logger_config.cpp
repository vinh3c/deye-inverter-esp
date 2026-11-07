#include "logger_config.h"
#include "logger_config_validation.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <esp_log.h>
#include <esp_matter_console.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace logger_config {
namespace {
constexpr char TAG[] = "logger_config";
constexpr char NAMESPACE[] = "deye_logger";
SemaphoreHandle_t settings_mutex = nullptr;

class SettingsLock {
public:
    SettingsLock() { configASSERT(settings_mutex); xSemaphoreTake(settings_mutex, portMAX_DELAY); }
    ~SettingsLock() { xSemaphoreGive(settings_mutex); }
};

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
        const auto error = get(settings);
        if (error == ESP_OK) {
            std::printf("LOGGER_CONFIG host=%s port=%u unit=1 profile=deye-single\n", settings.host, settings.port);
        } else {
            std::printf("LOGGER_CONFIG not configured: %s\n", esp_err_to_name(error));
        }
        return error == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : error;
    }
    // The request token lets the host distinguish a fresh commit acknowledgement
    // from old log output or the shell's echoed input.
    if (argc != 4 || std::strcmp(argv[0], "set") || !valid_host(argv[1]) || std::strlen(argv[3]) != 8)
        return ESP_ERR_INVALID_ARG;
    for (const char *c = argv[3]; *c; ++c) if (!std::isxdigit(static_cast<unsigned char>(*c))) return ESP_ERR_INVALID_ARG;
    uint16_t port;
    if (!parse_port(argv[2], port)) return ESP_ERR_INVALID_ARG;
    const auto error = set(argv[1], port);
    if (error != ESP_OK) return error;
    std::printf("LOGGER_CONFIG_SAVED token=%s host=%s port=%u\n", argv[3], argv[1], port);
    std::fflush(stdout);
    return ESP_OK;
}
} // namespace

esp_err_t get(Settings &settings)
{
    SettingsLock lock;
    return load(settings);
}

esp_err_t set(const char *host, uint16_t port)
{
    if (!host || !valid_host(host) || !port) return ESP_ERR_INVALID_ARG;
    SettingsLock lock;
    Settings settings{};
    settings.version = 1;
    std::strcpy(settings.host, host);
    settings.port = port;
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
    return ESP_OK;
}

void init()
{
    settings_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(settings_mutex ? ESP_OK : ESP_ERR_NO_MEM);
    Settings settings{};
    const auto error = load(settings);
    if (error == ESP_OK) {
        ESP_LOGI(TAG, "Logger host=%s port=%u unit=1 profile=deye-single", settings.host, settings.port);
    } else if (error == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "Logger not configured; readings unavailable");
    } else {
        ESP_LOGW(TAG, "Cannot load logger config: %s; readings unavailable", esp_err_to_name(error));
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
