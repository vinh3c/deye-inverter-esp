#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_heap_caps.h>
#include <esp_matter.h>
#include <esp_matter_console.h>
#include <nvs_flash.h>
#include <app/EventLogging.h>
#include <app/clusters/power-source-server/power-source-server.h>
#include <app/util/attribute-table.h>
#include <app/AttributeAccessInterfaceRegistry.h>
#include <app/AttributeValueEncoder.h>
#include <app/util/attribute-storage.h>
#include <app/server/CommissioningWindowManager.h>
#include <app/server/OnboardingCodesUtil.h>
#include <app/server/Server.h>

#include "device_identity.h"
#include "electrical_measurements.h"
#include "logger_config.h"
#include "logger_web.h"
#ifdef DEYE_SONOFF_BASIC_R4
#include "sonoff_relay.h"
#endif

namespace {
constexpr char TAG[] = "deye_sensors";
namespace ps = chip::app::Clusters::PowerSource;

// Matter TemperatureMeasurement values are signed hundredths of a degree Celsius.
struct Sensor {
    char name[24];
    const char *identity_key;
    int16_t minimum;
    int16_t maximum;
    deye::Index metric;
    uint16_t endpoint;
};

Sensor sensors[] = {
    {"AC Temperature", "AC", -10000, 20000, deye::acC, 0},
    {"DC Temperature", "DC", -10000, 20000, deye::dcC, 0},
    {"Battery Temperature", "Battery", -10000, 20000, deye::batteryC, 0},
};

struct ContactSensor {
    char name[8];
    bool closed;
    uint16_t endpoint;
};

ContactSensor grid{"Grid", false, 0};

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
    const chip::EndpointId children[] = {2, 3, 4, 5, 7, 8, 9, 10, 11,
#ifdef DEYE_SONOFF_BASIC_R4
        sonoff_relay::endpoint_id,
#endif
    };
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
        VerifyOrDie((esp_matter::cluster::get(sensor.endpoint, ps::Id) != nullptr) ==
                    (sensor.metric == deye::batteryC));
    }
    VerifyOrDie(!esp_matter::cluster::get(10, ps::Id));
}

void set_reachable(uint16_t endpoint, bool reachable)
{
    using namespace chip::app::Clusters;
    auto value = esp_matter_bool(reachable);
    ESP_ERROR_CHECK(esp_matter::attribute::update(endpoint, BridgedDeviceBasicInformation::Id,
        BridgedDeviceBasicInformation::Attributes::Reachable::Id, &value));
}

void update_battery_percent(const Sensor &sensor, const std::optional<int64_t> &percent)
{
    // Matter uses half-percent units: raw 0..200 represents 0..100%.
    // The shared decoder rejects unavailable/out-of-range register 184.
    const uint8_t expected = percent ? static_cast<uint8_t>(*percent * 2) : 0xff;
    auto value = esp_matter_nullable_uint8(percent ? nullable<uint8_t>(expected) : nullable<uint8_t>());
    ESP_ERROR_CHECK(esp_matter::attribute::update(sensor.endpoint, ps::Id,
        ps::Attributes::BatPercentRemaining::Id, &value));
    auto status = esp_matter_enum8(chip::to_underlying(percent ? ps::PowerSourceStatusEnum::kActive :
                                                              ps::PowerSourceStatusEnum::kUnavailable));
    ESP_ERROR_CHECK(esp_matter::attribute::update(sensor.endpoint, ps::Id,
        ps::Attributes::Status::Id, &status));
    uint8_t actual = 0;
    VerifyOrDie(emberAfReadAttribute(sensor.endpoint, ps::Id, ps::Attributes::BatPercentRemaining::Id,
        &actual, sizeof(actual)) == chip::Protocols::InteractionModel::Status::Success && actual == expected);
    if (percent) {
        ESP_LOGI(TAG, "BATTERY Battery Temperature endpoint=%u SOC=%lld %% register=184 Matter raw=%u read verified",
                 sensor.endpoint, static_cast<long long>(*percent), actual);
    } else {
        ESP_LOGI(TAG, "BATTERY Battery Temperature endpoint=%u SOC=null (unavailable) read verified", sensor.endpoint);
    }
}

void update_sensors(intptr_t context)
{
    using namespace chip::app::Clusters;
    // Network I/O completed on the worker; this callback only publishes Matter state.
    const std::unique_ptr<inverter_data::Snapshot> snapshot(reinterpret_cast<inverter_data::Snapshot *>(context));
    for (auto &sensor : sensors) {
        const auto reading = snapshot->get(sensor.metric);
        auto value = reading ? esp_matter_nullable_int16(static_cast<int16_t>(*reading)) :
                               esp_matter_nullable_int16(nullable<int16_t>());
        const auto error = esp_matter::attribute::update(sensor.endpoint, TemperatureMeasurement::Id,
            TemperatureMeasurement::Attributes::MeasuredValue::Id, &value);
        const bool battery = sensor.metric == deye::batteryC;
        const auto percent = battery ? snapshot->get(deye::batteryPercent) : std::nullopt;
        if (battery) update_battery_percent(sensor, percent);
        set_reachable(sensor.endpoint, reading.has_value() && error == ESP_OK &&
                                      (!battery || percent.has_value()));
        if (reading && error == ESP_OK) {
            ESP_LOGI(TAG, "TEMP %s endpoint=%u value=%.2f C register=%u (Modbus)",
                sensor.name, sensor.endpoint, *reading / 100.0, deye::METRICS[sensor.metric].address);
        } else {
            ESP_LOGW(TAG, "TEMP %s endpoint=%u unavailable", sensor.name, sensor.endpoint);
        }
    }
    const auto voltage = snapshot->get(deye::gridVoltageV);
    set_reachable(grid.endpoint, voltage.has_value());
    if (voltage) {
        // Voltage presence with hysteresis, not power or the grid relay status.
        const bool closed = *voltage >= (grid.closed ? 80000 : 100000);
        auto value = esp_matter_bool(closed);
        const auto error = esp_matter::attribute::update(grid.endpoint, BooleanState::Id,
            BooleanState::Attributes::StateValue::Id, &value);
        set_reachable(grid.endpoint, error == ESP_OK);
        if (error == ESP_OK) {
            if (closed != grid.closed) {
                chip::EventNumber number;
                BooleanState::Events::StateChange::Type event{closed};
                const auto event_error = chip::app::LogEvent(event, grid.endpoint, number);
                if (event_error != CHIP_NO_ERROR) ESP_LOGW(TAG, "Grid event: %s", event_error.AsString());
            }
            grid.closed = closed;
            ESP_LOGI(TAG, "CONTACT Grid endpoint=%u state=%s voltage=%.3f V (Modbus)",
                     grid.endpoint, closed ? "CLOSED" : "OPEN", *voltage / 1000.0);
        }
    } else {
        ESP_LOGW(TAG, "CONTACT Grid unavailable; retaining state, no outage event");
    }
    electrical_measurements::update(*snapshot);
    inverter_data::publication_complete();
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
    config.temperature_measurement.measured_value = nullable<int16_t>();
    config.temperature_measurement.min_measured_value = sensor.minimum;
    config.temperature_measurement.max_measured_value = sensor.maximum;
    auto *sensor_endpoint = endpoint::temperature_sensor::create(node, &config, ENDPOINT_FLAG_NONE, nullptr);
    attach_to_bridge(sensor_endpoint, bridge, sensor.name, mac, sensor.identity_key, sensor.identity_key);
    sensor.endpoint = endpoint::get_id(sensor_endpoint);
    if (sensor.metric == deye::batteryC) {
        // Battery metadata belongs to this existing temperature leaf;
        // temperature itself still reports register 182 in degrees Celsius.
        cluster::power_source::config_t source{};
        source.status = chip::to_underlying(ps::PowerSourceStatusEnum::kUnavailable);
        std::strcpy(source.description, "Inverter battery");
        auto *battery = cluster::power_source::create(sensor_endpoint, &source, CLUSTER_FLAG_SERVER,
            cluster::power_source::feature::battery::get_id());
        if (!battery || !cluster::power_source::attribute::create_bat_percent_remaining(
                battery, nullable<uint8_t>(), nullable<uint8_t>(0), nullable<uint8_t>(200))) {
            ESP_LOGE(TAG, "Cannot create Battery Temperature percentage");
            abort();
        }
    }

    ESP_LOGI(TAG, "TEMP %s endpoint=%u register=%u; awaiting Modbus", sensor.name,
             sensor.endpoint, deye::METRICS[sensor.metric].address);
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
    ESP_LOGI(TAG, "CONTACT Grid endpoint=%u awaiting Modbus voltage", grid.endpoint);
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
#ifdef DEYE_SONOFF_BASIC_R4
        ESP_LOGI(TAG, "PAIR window closed; hold the GPIO9 button for 5 seconds to reopen");
#else
        ESP_LOGI(TAG, "PAIR window closed; use matter esp factoryreset to start over");
#endif
        break;
    case DeviceEventType::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "NETWORK IP address changed");
        logger_web::log_address();
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
#ifdef DEYE_SONOFF_BASIC_R4
    sonoff_relay::init();
#endif
    // Do not silently erase fabrics/Wi-Fi if NVS is incompatible or full.
    ESP_ERROR_CHECK(nvs_flash_init());
    logger_config::init();

    esp_matter::node::config_t node_config{};
    std::strcpy(node_config.root_node.basic_information.node_label, device_identity::manufacturer);
    auto *node = esp_matter::node::create(&node_config,
#ifdef DEYE_SONOFF_BASIC_R4
        sonoff_relay::attribute_update,
#else
        nullptr,
#endif
        on_identify);
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
#ifdef DEYE_SONOFF_BASIC_R4
    char relay_name[] = "Relay";
    attach_to_bridge(sonoff_relay::create(node), bridge, relay_name, mac, "Relay");
#endif
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
#ifdef DEYE_SONOFF_BASIC_R4
    sonoff_relay::start();
    set_reachable(sonoff_relay::endpoint_id, true);
#endif
    for (const auto &sensor : sensors) {
        if (sensor.metric == deye::batteryC) {
            chip::EndpointId supplied[] = {sensor.endpoint};
            VerifyOrDie(chip::app::Clusters::PowerSourceServer::Instance().SetEndpointList(
                sensor.endpoint, chip::Span<chip::EndpointId>(supplied)) == CHIP_NO_ERROR);
        }
        set_reachable(sensor.endpoint, false);
        if (sensor.metric == deye::batteryC) update_battery_percent(sensor, std::nullopt);
    }
    set_reachable(grid.endpoint, false);
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

    ESP_LOGI(TAG, "MEMORY before services: free=%u largest=%u bytes",
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    inverter_data::start(update_sensors);
    logger_web::start();
    ESP_LOGI(TAG, "MEMORY after services: free=%u largest=%u bytes",
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    ESP_LOGI(TAG, "Modbus polling: deye-single unit=1, 6 sequential FC03 groups; 5 seconds after each cycle");
}
