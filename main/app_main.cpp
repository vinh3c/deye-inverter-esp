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
#include <app/AttributeAccessInterfaceRegistry.h>
#include <app/AttributeValueEncoder.h>
#include <app/util/attribute-storage.h>
#include <app/server/CommissioningWindowManager.h>
#include <app/server/OnboardingCodesUtil.h>
#include <app/server/Server.h>

#include "device_identity.h"
#include "electrical_measurements.h"
#include "logger_config.h"

namespace {
constexpr char TAG[] = "deye_sensors";
constexpr uint32_t UPDATE_INTERVAL_MS = 5000;

// Matter TemperatureMeasurement values are signed hundredths of a degree Celsius.
struct Sensor {
    char name[24];
    const char *identity_key;
    int16_t minimum;
    int16_t maximum;
    int16_t temperature;
    uint16_t endpoint;
};

Sensor sensors[] = {
    {"AC Temperature", "AC", 2500, 5500, 3200, 0},
    {"DC Temperature", "DC", 3000, 6500, 3600, 0},
    {"Battery Temperature", "Battery", 2000, 4500, 2800, 0},
};

struct ContactSensor {
    char name[8];
    bool closed;
    uint16_t endpoint;
};

ContactSensor grid{"Grid", true, 0};

void verify_parts_list(chip::EndpointId endpoint, const chip::EndpointId *expected, size_t count)
{
    using namespace chip::app;
    uint8_t buffer[512];
    chip::TLV::TLVWriter writer;
    writer.Init(buffer);
    chip::TLV::TLVType outer;
    VerifyOrDie(writer.StartContainer(chip::TLV::AnonymousTag(), chip::TLV::kTLVType_Structure, outer) == CHIP_NO_ERROR);
    AttributeReportIBs::Builder reports;
    VerifyOrDie(reports.Init(&writer, 1) == CHIP_NO_ERROR);
    chip::Access::SubjectDescriptor subject;
    subject.authMode = chip::Access::AuthMode::kPase;
    ConcreteReadAttributePath path(endpoint, Clusters::Descriptor::Id, Clusters::Descriptor::Attributes::PartsList::Id);
    AttributeValueEncoder encoder(reports, subject, path, 1);
    auto *access = AttributeAccessInterfaceRegistry::Instance().Get(endpoint, path.mClusterId);
    VerifyOrDie(access && access->Read(path, encoder) == CHIP_NO_ERROR && encoder.TriedEncode());
    VerifyOrDie(reports.EndOfAttributeReportIBs() == CHIP_NO_ERROR);
    VerifyOrDie(writer.EndContainer(outer) == CHIP_NO_ERROR && writer.Finalize() == CHIP_NO_ERROR);

    // Decode the actual Descriptor response: a leaf must encode an empty array,
    // not silently fall back to the SDK's unset backing attribute.
    chip::TLV::TLVReader reader;
    reader.Init(buffer, writer.GetLengthWritten());
    size_t arrays = 0;
    auto inspect = [&](auto &&self) -> void {
        CHIP_ERROR error;
        while ((error = reader.Next()) == CHIP_NO_ERROR) {
            const auto type = reader.GetType();
            if (!chip::TLV::TLVTypeIsContainer(type)) continue;
            const bool parts = type == chip::TLV::kTLVType_Array && reader.GetTag() == chip::TLV::ContextTag(2);
            chip::TLV::TLVType container;
            VerifyOrDie(reader.EnterContainer(container) == CHIP_NO_ERROR);
            if (parts) {
                ++arrays;
                size_t index = 0;
                while ((error = reader.Next()) == CHIP_NO_ERROR) {
                    chip::EndpointId child;
                    VerifyOrDie(index < count && reader.Get(child) == CHIP_NO_ERROR && child == expected[index]);
                    ++index;
                }
                VerifyOrDie(error == CHIP_END_OF_TLV && index == count);
            } else {
                self(self);
            }
            VerifyOrDie(reader.ExitContainer(container) == CHIP_NO_ERROR);
        }
        VerifyOrDie(error == CHIP_END_OF_TLV);
    };
    inspect(inspect);
    VerifyOrDie(arrays == 1);
    ESP_LOGI(TAG, "TOPOLOGY endpoint=%u PartsList read/encoding verified: %u children", endpoint, static_cast<unsigned>(count));
}

void configure_bridge_topology()
{
    const chip::EndpointId children[] = {2, 3, 4, 5, 7, 8, 9, 10, 11};
    // Dynamic endpoints do not receive a composition mode from this SDK.
    // Explicit tree composition makes bridge children siblings and leaves empty.
    VerifyOrDie(chip::app::SetTreeCompositionForEndpoint(1) == CHIP_NO_ERROR);
    for (const auto endpoint : children) {
        VerifyOrDie(chip::app::SetTreeCompositionForEndpoint(endpoint) == CHIP_NO_ERROR);
        VerifyOrDie(emberAfParentEndpointFromIndex(emberAfIndexFromEndpoint(endpoint)) == 1);
        verify_parts_list(endpoint, nullptr, 0);
    }
    verify_parts_list(1, children, sizeof(children) / sizeof(children[0]));
    for (const auto &sensor : sensors) {
        VerifyOrDie(!esp_matter::cluster::get(sensor.endpoint, chip::app::Clusters::ElectricalPowerMeasurement::Id));
        VerifyOrDie(!esp_matter::cluster::get(sensor.endpoint, chip::app::Clusters::PowerTopology::Id));
    }
}

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
    electrical_measurements::update(grid.closed);
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
                      char *name, const uint8_t (&mac)[6], const char *identity_key = nullptr,
                      const char *previous_label = nullptr)
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
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], identity_key ? identity_key : name);
    auto *label = basic ? cluster::bridged_device_basic_information::attribute::create_node_label(
                             basic, name, std::strlen(name)) : nullptr;
    if (!label) {
        ESP_LOGE(TAG, "Cannot set %s sensor identity", name);
        abort();
    }
    // Migrate only our old default names, preserving controller/user-customized labels.
    if (previous_label && std::strcmp(name, previous_label) != 0) {
        auto saved = esp_matter_invalid(nullptr);
        ESP_ERROR_CHECK(attribute::get_val(label, &saved));
        if (saved.type == ESP_MATTER_VAL_TYPE_CHAR_STRING && saved.val.a.b &&
            saved.val.a.s == std::strlen(previous_label) &&
            std::memcmp(saved.val.a.b, previous_label, saved.val.a.s) == 0) {
            auto renamed = esp_matter_char_str(name, std::strlen(name));
            ESP_ERROR_CHECK(attribute::set_val(label, &renamed));
            ESP_LOGI(TAG, "LABEL endpoint=%u renamed %s -> %s",
                     endpoint::get_id(sensor_endpoint), previous_label, name);
        }
    }
    // bridged_node::add already creates UniqueID; set its value instead of
    // calling create again (which would leave the existing empty value intact).
    auto *id = attribute::get(basic, chip::app::Clusters::BridgedDeviceBasicInformation::Attributes::UniqueID::Id);
    auto id_value = esp_matter_char_str(unique_id, std::strlen(unique_id));
    ESP_ERROR_CHECK(attribute::set_val(id, &id_value));
    char serial[33];
    device_identity::serial_number(serial, sizeof(serial), mac, identity_key ? identity_key : name);
    if (!cluster::bridged_device_basic_information::attribute::create_vendor_name(
            basic, device_identity::manufacturer, std::strlen(device_identity::manufacturer)) ||
        !cluster::bridged_device_basic_information::attribute::create_product_name(
            basic, device_identity::model, std::strlen(device_identity::model)) ||
        !cluster::bridged_device_basic_information::attribute::create_serial_number(basic, serial, std::strlen(serial))) {
        ESP_LOGE(TAG, "Cannot set %s manufacturer/model/serial", name);
        abort();
    }
    ESP_LOGI(TAG, "IDENTITY %s manufacturer=%s model=%s serial=%s", name,
             device_identity::manufacturer, device_identity::model, serial);
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
    attach_to_bridge(sensor_endpoint, bridge, sensor.name, mac, sensor.identity_key, sensor.identity_key);
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
        ESP_LOGI(TAG, "PAIR complete; temperature, contact and electrical power sensors are ready");
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
    logger_config::init();

    esp_matter::node::config_t node_config{};
    std::strcpy(node_config.root_node.basic_information.node_label, device_identity::manufacturer);
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
    electrical_measurements::create(node, bridge, mac, attach_to_bridge);
    auto *basic = esp_matter::cluster::get(static_cast<uint16_t>(0), chip::app::Clusters::BasicInformation::Id);
    char serial[33];
    device_identity::serial_number(serial, sizeof(serial), mac);
    if (!esp_matter::cluster::basic_information::attribute::create_serial_number(basic, serial, std::strlen(serial))) {
        ESP_LOGE(TAG, "Cannot create root serial number");
        abort();
    }
    ESP_ERROR_CHECK(esp_matter::start(on_event));

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    device_identity::install_provider(mac);
    electrical_measurements::start(node);
    configure_bridge_topology();
    if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
        PrintOnboardingCodes(chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));
    }
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

#if CONFIG_ENABLE_CHIP_SHELL
    ESP_ERROR_CHECK(esp_matter::console::diagnostics_register_commands());
    ESP_ERROR_CHECK(esp_matter::console::factoryreset_register_commands());
    logger_config::register_console();
    ESP_ERROR_CHECK(esp_matter::console::init());
#endif

    if (xTaskCreate(simulation_task, "sensor_sim", 3072, nullptr, 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Cannot start sensor simulation");
        abort();
    }
    ESP_LOGI(TAG, "Simulation running: 3 temperatures + Grid contact + 5 power sensors, update every %lu ms",
             static_cast<unsigned long>(UPDATE_INTERVAL_MS));
}
