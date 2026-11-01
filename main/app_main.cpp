#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_matter.h>
#include <esp_matter_console.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <app/EventLogging.h>
#include <app/server/CommissioningWindowManager.h>
#include <app/server/OnboardingCodesUtil.h>
#include <app/server/Server.h>

namespace {
constexpr char TAG[] = "deye_sensors";
constexpr uint32_t UPDATE_INTERVAL_MS = 5000;

// Matter TemperatureMeasurement values are signed hundredths of a degree Celsius.
struct Sensor {
    char name[8];
    int16_t minimum;
    int16_t maximum;
    int16_t temperature;
    uint16_t endpoint;
};

Sensor sensors[] = {
    {"AC", 2500, 5500, 3200, 0},
    {"DC", 3000, 6500, 3600, 0},
    {"Battery", 2000, 4500, 2800, 0},
};

struct ContactSensor {
    char name[8];
    bool closed;
    uint16_t endpoint;
};

ContactSensor grid{"Grid", true, 0};

void update_sensors(intptr_t)
{
    using namespace chip::app::Clusters;
    // ScheduleWork runs on the Matter thread, where attribute updates are safe.
    for (auto &sensor : sensors) {
        const int step = static_cast<int>(esp_random() % 101) - 50;
        const int16_t next = static_cast<int16_t>(std::clamp(
            sensor.temperature + step, static_cast<int>(sensor.minimum), static_cast<int>(sensor.maximum)));
        auto value = esp_matter_nullable_int16(next);
        const esp_err_t error = esp_matter::attribute::update(sensor.endpoint,
            TemperatureMeasurement::Id, TemperatureMeasurement::Attributes::MeasuredValue::Id, &value);
        if (error == ESP_OK) {
            sensor.temperature = next;
            ESP_LOGI(TAG, "TEMP %s endpoint=%u value=%.2f C (simulated)",
                     sensor.name, sensor.endpoint, sensor.temperature / 100.0);
        } else {
            ESP_LOGE(TAG, "TEMP %s update failed: %s", sensor.name, esp_err_to_name(error));
        }
    }

    // For a Matter Contact Sensor, true means closed/contact; false means open.
    const bool closed = (esp_random() & 1u) != 0;
    auto value = esp_matter_bool(closed);
    const esp_err_t error = esp_matter::attribute::update(grid.endpoint,
        BooleanState::Id, BooleanState::Attributes::StateValue::Id, &value);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "CONTACT Grid update failed: %s", esp_err_to_name(error));
        return;
    }
    const bool changed = closed != grid.closed;
    grid.closed = closed;
    if (changed) {
        chip::EventNumber event_number;
        BooleanState::Events::StateChange::Type event{closed};
        const auto event_error = chip::app::LogEvent(event, grid.endpoint, event_number);
        if (event_error != CHIP_NO_ERROR) {
            ESP_LOGE(TAG, "CONTACT Grid event failed: %s", event_error.AsString());
        }
    }
    ESP_LOGI(TAG, "CONTACT Grid endpoint=%u state=%s (simulated)",
             grid.endpoint, grid.closed ? "CLOSED" : "OPEN");
}

void simulation_task(void *)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(UPDATE_INTERVAL_MS));
        const auto error = chip::DeviceLayer::PlatformMgr().ScheduleWork(update_sensors, 0);
        if (error != CHIP_NO_ERROR) {
            ESP_LOGE(TAG, "Cannot schedule sensor update: %s", error.AsString());
        }
    }
}

void attach_to_bridge(esp_matter::endpoint_t *sensor_endpoint, esp_matter::endpoint_t *bridge,
                      char *name, const uint8_t (&mac)[6])
{
    using namespace esp_matter;
    if (!sensor_endpoint) {
        ESP_LOGE(TAG, "Cannot create %s sensor", name);
        abort();
    }
    endpoint::bridged_node::config_t bridged_config{};
    ESP_ERROR_CHECK(endpoint::bridged_node::add(sensor_endpoint, &bridged_config));
    ESP_ERROR_CHECK(endpoint::set_parent_endpoint(sensor_endpoint, bridge));
    auto *basic = cluster::get(sensor_endpoint, chip::app::Clusters::BridgedDeviceBasicInformation::Id);
    char unique_id[33];
    std::snprintf(unique_id, sizeof(unique_id), "deye-%02x%02x%02x%02x%02x%02x-%.7s",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], name);
    if (!basic || !cluster::bridged_device_basic_information::attribute::create_node_label(
                      basic, name, std::strlen(name))) {
        ESP_LOGE(TAG, "Cannot set %s sensor identity", name);
        abort();
    }
    // bridged_node::add already creates UniqueID; set its value instead of
    // calling create again (which would leave the existing empty value intact).
    auto *id = attribute::get(basic, chip::app::Clusters::BridgedDeviceBasicInformation::Attributes::UniqueID::Id);
    auto id_value = esp_matter_char_str(unique_id, std::strlen(unique_id));
    ESP_ERROR_CHECK(attribute::set_val(id, &id_value));
}

void create_sensor(esp_matter::node_t *node, esp_matter::endpoint_t *bridge, Sensor &sensor,
                   const uint8_t (&mac)[6])
{
    using namespace esp_matter;
    endpoint::temperature_sensor::config_t config{};
    config.temperature_measurement.measured_value = sensor.temperature;
    config.temperature_measurement.min_measured_value = sensor.minimum;
    config.temperature_measurement.max_measured_value = sensor.maximum;
    auto *sensor_endpoint = endpoint::temperature_sensor::create(node, &config, ENDPOINT_FLAG_NONE, nullptr);
    attach_to_bridge(sensor_endpoint, bridge, sensor.name, mac);
    sensor.endpoint = endpoint::get_id(sensor_endpoint);
    ESP_LOGI(TAG, "TEMP %s endpoint=%u initial=%.2f C range=%.2f..%.2f C (simulated)",
             sensor.name, sensor.endpoint, sensor.temperature / 100.0,
             sensor.minimum / 100.0, sensor.maximum / 100.0);
}

void create_grid_sensor(esp_matter::node_t *node, esp_matter::endpoint_t *bridge,
                        const uint8_t (&mac)[6])
{
    using namespace esp_matter;
    endpoint::contact_sensor::config_t config{};
    config.boolean_state.state_value = grid.closed;
    auto *sensor_endpoint = endpoint::contact_sensor::create(node, &config, ENDPOINT_FLAG_NONE, nullptr);
    attach_to_bridge(sensor_endpoint, bridge, grid.name, mac);
    auto *state = cluster::get(sensor_endpoint, chip::app::Clusters::BooleanState::Id);
    if (!cluster::boolean_state::event::create_state_change(state)) {
        ESP_LOGE(TAG, "Cannot create Grid StateChange event");
        abort();
    }
    grid.endpoint = endpoint::get_id(sensor_endpoint);
    ESP_LOGI(TAG, "CONTACT Grid endpoint=%u initial=CLOSED (simulated)", grid.endpoint);
}

esp_err_t on_identify(esp_matter::identification::callback_type_t type, uint16_t endpoint,
                      uint8_t effect, uint8_t variant, void *)
{
    ESP_LOGI(TAG, "IDENTIFY endpoint=%u type=%u effect=%u variant=%u", endpoint,
             static_cast<unsigned>(type), effect, variant);
    return ESP_OK;
}

void on_event(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t)
{
    using namespace chip::DeviceLayer;
    switch (event->Type) {
    case DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(TAG, "PAIR session started");
        break;
    case DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "PAIR complete; AC, DC, Battery temperatures and Grid contact are ready");
        break;
    case DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGW(TAG, "PAIR timed out; retry while commissioning window is open");
        break;
    case DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "PAIR window open");
        break;
    case DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "PAIR window closed; use matter esp factoryreset to start over");
        break;
    case DeviceEventType::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "NETWORK IP address changed");
        break;
    case DeviceEventType::kFabricRemoved: {
        auto &server = chip::Server::GetInstance();
        ESP_LOGI(TAG, "PAIR fabric removed; remaining=%u",
                 static_cast<unsigned>(server.GetFabricTable().FabricCount()));
        if (server.GetFabricTable().FabricCount() == 0) {
            auto &window = server.GetCommissioningWindowManager();
            if (!window.IsCommissioningWindowOpen()) {
                // Wi-Fi credentials remain after fabric removal; advertise over IP.
                const auto error = window.OpenBasicCommissioningWindow(
                    chip::System::Clock::Seconds16(300), chip::CommissioningWindowAdvertisement::kDnssdOnly);
                if (error != CHIP_NO_ERROR) {
                    ESP_LOGE(TAG, "Cannot reopen pairing: %s", error.AsString());
                }
            }
        }
        break;
    }
    default:
        break;
    }
}
} // namespace

extern "C" void app_main()
{
    // Do not silently erase fabrics/Wi-Fi if NVS is incompatible or full.
    ESP_ERROR_CHECK(nvs_flash_init());

    esp_matter::node::config_t node_config{};
    auto *node = esp_matter::node::create(&node_config, nullptr, on_identify);
    if (!node) {
        ESP_LOGE(TAG, "Cannot create Matter node");
        abort();
    }

    esp_matter::endpoint::aggregator::config_t bridge_config{};
    auto *bridge = esp_matter::endpoint::aggregator::create(
        node, &bridge_config, esp_matter::ENDPOINT_FLAG_NONE, nullptr);
    if (!bridge) {
        ESP_LOGE(TAG, "Cannot create sensor bridge");
        abort();
    }
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    for (auto &sensor : sensors) {
        create_sensor(node, bridge, sensor, mac);
    }
    create_grid_sensor(node, bridge, mac);
    ESP_ERROR_CHECK(esp_matter::start(on_event));

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
        PrintOnboardingCodes(chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));
    }
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

#if CONFIG_ENABLE_CHIP_SHELL
    ESP_ERROR_CHECK(esp_matter::console::diagnostics_register_commands());
    ESP_ERROR_CHECK(esp_matter::console::factoryreset_register_commands());
    ESP_ERROR_CHECK(esp_matter::console::init());
#endif

    if (xTaskCreate(simulation_task, "sensor_sim", 3072, nullptr, 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Cannot start sensor simulation");
        abort();
    }
    ESP_LOGI(TAG, "Simulation running: 3 temperature sensors + Grid contact, update every %lu ms",
             static_cast<unsigned long>(UPDATE_INTERVAL_MS));
}
