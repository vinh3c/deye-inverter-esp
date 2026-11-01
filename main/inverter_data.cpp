#include "inverter_data.h"
#include "logger_config.h"

#include <cerrno>
#include <cstdio>
#include <memory>
#include <new>
#include <fcntl.h>
#include <unistd.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <platform/CHIPDeviceLayer.h>

namespace inverter_data {
namespace {
constexpr char TAG[] = "deye_modbus";
constexpr uint32_t TIMEOUT_MS = 3000;
constexpr uint32_t INTERVAL_MS = 5000;
Callback publish;
uint16_t transaction = 0;

int64_t now_ms() { return esp_timer_get_time() / 1000; }

bool wait_socket(int socket, bool writing, int64_t deadline)
{
    for (;;) {
        const auto remaining = deadline - now_ms();
        if (remaining <= 0) return false;
        timeval timeout{static_cast<time_t>(remaining / 1000), static_cast<suseconds_t>((remaining % 1000) * 1000)};
        fd_set ready;
        FD_ZERO(&ready); FD_SET(socket, &ready);
        const int result = select(socket + 1, writing ? nullptr : &ready, writing ? &ready : nullptr, nullptr, &timeout);
        if (result >= 0) return result > 0;
        if (errno != EINTR) return false;
    }
}

bool transfer(int socket, uint8_t *data, size_t length, bool writing, int64_t deadline)
{
    while (length) {
        if (!wait_socket(socket, writing, deadline)) return false;
        const auto count = writing ? send(socket, data, length, 0) : recv(socket, data, length, 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) return false;
        data += count; length -= count;
    }
    return true;
}

bool read_group(const logger_config::Settings &settings, deye::Group group, uint16_t *words)
{
    char service[6];
    std::snprintf(service, sizeof(service), "%u", settings.port);
    addrinfo hints{}, *addresses = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(settings.host, service, &hints, &addresses) != 0) return false;
    const auto deadline = now_ms() + TIMEOUT_MS;
    int socket = -1;
    for (auto *address = addresses; address; address = address->ai_next) {
        socket = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket < 0) continue;
        const int flags = fcntl(socket, F_GETFL, 0);
        if (flags < 0 || fcntl(socket, F_SETFL, flags | O_NONBLOCK) < 0) { close(socket); socket = -1; continue; }
        const int connected = connect(socket, address->ai_addr, address->ai_addrlen);
        int error = 0;
        socklen_t size = sizeof(error);
        if (connected == 0 || (errno == EINPROGRESS && wait_socket(socket, true, deadline) &&
            getsockopt(socket, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0)) break;
        close(socket); socket = -1;
    }
    freeaddrinfo(addresses);
    if (socket < 0) return false;
    uint8_t request[12]{};
    const uint16_t id = ++transaction;
    deye::request(request, id, deye::UNIT_ID, group);
    uint8_t response[260]{};
    bool okay = transfer(socket, request, sizeof(request), true, deadline) &&
                transfer(socket, response, 7, false, deadline) && deye::valid_header(response, id, deye::UNIT_ID);
    if (okay) {
        const size_t body_length = deye::be16(response + 4) - 1;
        okay = transfer(socket, response + 7, body_length, false, deadline);
        if (okay && response[7] == 0x83) {
            ESP_LOGW(TAG, "FC03 exception=0x%02x registers=%u..%u", response[8], group.start, group.start + group.count - 1);
        }
        okay = okay && deye::valid_response(response, body_length + 7, group.count);
    }
    close(socket);
    if (okay) for (size_t i = 0; i < group.count; ++i) words[i] = deye::be16(response + 9 + i * 2);
    return okay;
}

void task(void *)
{
    for (;;) {
        auto snapshot = std::unique_ptr<Snapshot>(new (std::nothrow) Snapshot{});
        if (!snapshot) { vTaskDelay(pdMS_TO_TICKS(INTERVAL_MS)); continue; }
        logger_config::Settings settings{};
        const auto started = now_ms();
        size_t successes = 0;
        if (logger_config::get(settings) == ESP_OK) {
            // One connection per compact group matches the installed logger's
            // proven behavior. Never pipeline or overlap requests.
            for (const auto group : deye::GROUPS) {
                uint16_t words[125]{};
                const auto group_start = now_ms();
                const bool okay = read_group(settings, group, words);
                ESP_LOGI(TAG, "FC03 %s:%u unit=%u registers=%u..%u status=%s elapsed_ms=%lld",
                    settings.host, settings.port, deye::UNIT_ID, group.start, group.start + group.count - 1,
                    okay ? "ok" : "error", static_cast<long long>(now_ms() - group_start));
                if (!okay) continue;
                ++successes;
                for (size_t i = 0; i < deye::METRIC_COUNT; ++i) {
                    const auto address = deye::METRICS[i].address;
                    if (address >= group.start && address < group.start + group.count)
                        snapshot->raw[i] = words[address - group.start];
                }
            }
        }
        ESP_LOGI(TAG, "POLL completed groups_ok=%u/6 elapsed_ms=%lld", static_cast<unsigned>(successes),
                 static_cast<long long>(now_ms() - started));
        for (size_t i = 0; i < deye::METRIC_COUNT; ++i) {
            const auto decoded = deye::decode(deye::METRICS[i], snapshot->raw[i]);
            if (snapshot->raw[i] && decoded) {
                ESP_LOGI(TAG, "RAW %s register=%u word=%u scaled=%lld", deye::METRICS[i].key,
                    deye::METRICS[i].address, *snapshot->raw[i], static_cast<long long>(*decoded));
            } else {
                ESP_LOGW(TAG, "RAW %s register=%u unavailable", deye::METRICS[i].key, deye::METRICS[i].address);
            }
        }
        // A slow Matter loop can queue at most one snapshot: the worker waits
        // until publication finishes before beginning the next interval.
        auto *pending = snapshot.release();
        const auto error = chip::DeviceLayer::PlatformMgr().ScheduleWork(publish, reinterpret_cast<intptr_t>(pending));
        if (error != CHIP_NO_ERROR) {
            delete pending;
            ESP_LOGW(TAG, "Cannot schedule Matter publication: %s", error.AsString());
        } else {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
        vTaskDelay(pdMS_TO_TICKS(INTERVAL_MS));
    }
}
TaskHandle_t worker = nullptr;
} // namespace

void publication_complete() { xTaskNotifyGive(worker); }

void start(Callback callback)
{
    publish = callback;
    if (xTaskCreate(task, "deye_modbus", 6144, nullptr, 4, &worker) != pdPASS) abort();
}
} // namespace inverter_data
