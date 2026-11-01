#include "electrical_measurements.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <esp_log.h>
#include <app/AttributeAccessInterfaceRegistry.h>
#include <app/AttributeValueEncoder.h>
#include <app/util/attribute-table.h>
#include <app/clusters/electrical-power-measurement-server/electrical-power-measurement-server.h>
#include <app/reporting/reporting.h>

namespace electrical_measurements {
namespace {
namespace epm = chip::app::Clusters::ElectricalPowerMeasurement;
namespace dm = chip::app::DataModel;
using Reading = dm::Nullable<int64_t>;
constexpr char TAG[] = "deye_sensors";

struct Profile {
    const char *name;
    const char *key;
    uint16_t endpoint;
    bool ac;
    deye::Index voltage;
    deye::Index current;
    deye::Index power;
};

class Channel final : public epm::Delegate {
public:
    explicit Channel(Profile profile) : profile_(profile)
    {
        // Register representation bounds, not a claim of calibrated accuracy.
        const auto &v = deye::METRICS[profile.voltage];
        const auto &i = deye::METRICS[profile.current];
        const auto &p = deye::METRICS[profile.power];
        const int64_t minima[] = {0, i.is_signed ? -32768LL * i.multiplier : 0, p.is_signed ? -32768LL * p.multiplier : 0};
        const int64_t maxima[] = {65534LL * v.multiplier,
            (i.is_signed ? 32767LL : 65534LL) * i.multiplier, (p.is_signed ? 32767LL : 65534LL) * p.multiplier};
        const epm::MeasurementTypeEnum types[] = {
            epm::MeasurementTypeEnum::kVoltage,
            epm::MeasurementTypeEnum::kActiveCurrent,
            epm::MeasurementTypeEnum::kActivePower,
        };
        for (size_t i = 0; i < 3; ++i) {
            ranges_[i].rangeMin = minima[i];
            ranges_[i].rangeMax = maxima[i];
            // No calibrated logger tolerance is known: advertise only a
            // conservative full-span bound rather than invented precision.
            ranges_[i].fixedMax.SetValue(maxima[i] - minima[i]);
            accuracy_[i].measurementType = types[i];
            accuracy_[i].measured = true;
            accuracy_[i].minMeasuredValue = minima[i];
            accuracy_[i].maxMeasuredValue = maxima[i];
            accuracy_[i].accuracyRanges = dm::List<const epm::Structs::MeasurementAccuracyRangeStruct::Type>(&ranges_[i], 1);
        }
    }

    void create(esp_matter::node_t *node, esp_matter::endpoint_t *bridge,
                const uint8_t (&mac)[6], AttachToBridge attach)
    {
        using namespace esp_matter;
        auto *endpoint = endpoint::create(node, ENDPOINT_FLAG_NONE, nullptr);
        if (!endpoint || endpoint::get_id(endpoint) != profile_.endpoint) {
            ESP_LOGE(TAG, "Cannot allocate stable endpoint %u for %s", profile_.endpoint, profile_.name);
            abort();
        }
        cluster::descriptor::config_t descriptor{};
        cluster::power_topology::config_t topology{};
        cluster::electrical_power_measurement::config_t measurement{};
        // Build the Electrical Sensor device explicitly: the SDK convenience
        // helper enables both AC and DC, whereas these channels each use one.
        ESP_ERROR_CHECK(endpoint::add_device_type(endpoint, endpoint::electrical_sensor::get_device_type_id(),
                                                 endpoint::electrical_sensor::get_device_type_version()));
        auto *description = cluster::descriptor::create(endpoint, &descriptor, CLUSTER_FLAG_SERVER);
        // Each reading belongs only to this leaf. NODE would advertise it for
        // the entire Matter node, including unrelated temperature sensors.
        auto *topology_cluster = cluster::power_topology::create(endpoint, &topology, CLUSTER_FLAG_SERVER,
            cluster::power_topology::feature::tree_topology::get_id());
        auto *power = cluster::electrical_power_measurement::create(endpoint, &measurement, CLUSTER_FLAG_SERVER,
            chip::to_underlying(profile_.ac ? epm::Feature::kAlternatingCurrent : epm::Feature::kDirectCurrent));
        if (!description || !topology_cluster || !power ||
            !cluster::electrical_power_measurement::attribute::create_voltage(power, nullable<int64_t>()) ||
            !cluster::electrical_power_measurement::attribute::create_active_current(power, nullable<int64_t>())) {
            ESP_LOGE(TAG, "Cannot create %s ElectricalPowerMeasurement attributes", profile_.name);
            abort();
        }
        char label[24];
        std::strcpy(label, profile_.name);
        attach(endpoint, bridge, label, mac, profile_.key, nullptr);
    }

    void start()
    {
        uint32_t topology_features = 0;
        const auto topology_status = emberAfReadAttribute(profile_.endpoint,
            chip::app::Clusters::PowerTopology::Id, chip::app::Clusters::PowerTopology::Attributes::FeatureMap::Id,
            reinterpret_cast<uint8_t *>(&topology_features), sizeof(topology_features));
        VerifyOrDie(topology_status == chip::Protocols::InteractionModel::Status::Success &&
                    topology_features == chip::to_underlying(chip::app::Clusters::PowerTopology::Feature::kTreeTopology));
        ESP_LOGI(TAG, "POWER %s PowerTopology TREE verified: endpoint-local scope (FeatureMap=0x%lx)",
                 profile_.name, static_cast<unsigned long>(topology_features));
        chip::BitMask<epm::OptionalAttributes> optional;
        optional.Set(epm::OptionalAttributes::kOptionalAttributeVoltage);
        optional.Set(epm::OptionalAttributes::kOptionalAttributeActiveCurrent);
        // Register and retain our own Instance. The SDK's default callback uses
        // an empty optional-attribute mask, which rejects Voltage/ActiveCurrent.
        instance_ = std::make_unique<epm::Instance>(profile_.endpoint, *this,
            chip::BitMask<epm::Feature>(profile_.ac ? epm::Feature::kAlternatingCurrent : epm::Feature::kDirectCurrent), optional);
        const auto error = instance_->Init();
        if (error != CHIP_NO_ERROR ||
            chip::app::AttributeAccessInterfaceRegistry::Instance().Get(profile_.endpoint, epm::Id) != instance_.get()) {
            ESP_LOGE(TAG, "Cannot register %s power measurement delegate: %s", profile_.name, error.AsString());
            abort();
        }
        verify_reads();
        update(inverter_data::Snapshot{});
    }

    void update(const inverter_data::Snapshot &snapshot)
    {
        voltage_ = snapshot.get(profile_.voltage);
        current_ = snapshot.get(profile_.current);
        power_ = snapshot.get(profile_.power);
        // Delegate-backed values must dirty the reporting engine explicitly.
        for (auto id : {epm::Attributes::Voltage::Id, epm::Attributes::ActiveCurrent::Id, epm::Attributes::ActivePower::Id}) {
            MatterReportingAttributeChangeCallback(profile_.endpoint, epm::Id, id);
        }
        auto reachable = esp_matter_bool(voltage_.has_value() && current_.has_value() && power_.has_value());
        ESP_ERROR_CHECK(esp_matter::attribute::update(profile_.endpoint,
            chip::app::Clusters::BridgedDeviceBasicInformation::Id,
            chip::app::Clusters::BridgedDeviceBasicInformation::Attributes::Reachable::Id, &reachable));
        if (!verified_live_ && voltage_ && current_ && power_) {
            verify_reads();
            verified_live_ = true;
            ESP_LOGI(TAG, "POWER %s live Modbus values passed Matter read/encoding", profile_.name);
        }
        log();
    }

    epm::PowerModeEnum GetPowerMode() override { return profile_.ac ? epm::PowerModeEnum::kAc : epm::PowerModeEnum::kDc; }
    uint8_t GetNumberOfMeasurementTypes() override { return 3; }
    CHIP_ERROR StartAccuracyRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetAccuracyByIndex(uint8_t index, epm::Structs::MeasurementAccuracyStruct::Type &value) override
    {
        if (index >= 3) return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        value = accuracy_[index];
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR EndAccuracyRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR StartRangesRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetRangeByIndex(uint8_t, epm::Structs::MeasurementRangeStruct::Type &) override { return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED; }
    CHIP_ERROR EndRangesRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR StartHarmonicCurrentsRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetHarmonicCurrentsByIndex(uint8_t, epm::Structs::HarmonicMeasurementStruct::Type &) override { return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED; }
    CHIP_ERROR EndHarmonicCurrentsRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR StartHarmonicPhasesRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetHarmonicPhasesByIndex(uint8_t, epm::Structs::HarmonicMeasurementStruct::Type &) override { return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED; }
    CHIP_ERROR EndHarmonicPhasesRead() override { return CHIP_NO_ERROR; }
    Reading GetVoltage() override { return voltage_ ? dm::MakeNullable(*voltage_) : Reading(); }
    Reading GetActiveCurrent() override { return current_ ? dm::MakeNullable(*current_) : Reading(); }
    Reading GetActivePower() override { return power_ ? dm::MakeNullable(*power_) : Reading(); }
    Reading GetReactiveCurrent() override { return Reading(); }
    Reading GetApparentCurrent() override { return Reading(); }
    Reading GetReactivePower() override { return Reading(); }
    Reading GetApparentPower() override { return Reading(); }
    Reading GetRMSVoltage() override { return Reading(); }
    Reading GetRMSCurrent() override { return Reading(); }
    Reading GetRMSPower() override { return Reading(); }
    Reading GetFrequency() override { return Reading(); }
    Reading GetPowerFactor() override { return Reading(); }
    Reading GetNeutralCurrent() override { return Reading(); }

private:
    CHIP_ERROR encode_read(chip::AttributeId id)
    {
        // Exercise the same AAI/encoder used by an actual Matter read, not just
        // the getters. This catches incorrect optional masks or lists.
        uint8_t buffer[512];
        chip::TLV::TLVWriter writer;
        writer.Init(buffer);
        chip::TLV::TLVType outer;
        ReturnErrorOnFailure(writer.StartContainer(chip::TLV::AnonymousTag(), chip::TLV::kTLVType_Structure, outer));
        chip::app::AttributeReportIBs::Builder reports;
        ReturnErrorOnFailure(reports.Init(&writer, 1));
        chip::Access::SubjectDescriptor subject;
        subject.authMode = chip::Access::AuthMode::kPase;
        chip::app::ConcreteReadAttributePath path(profile_.endpoint, epm::Id, id);
        chip::app::AttributeValueEncoder encoder(reports, subject, path, 1);
        auto *access = chip::app::AttributeAccessInterfaceRegistry::Instance().Get(profile_.endpoint, epm::Id);
        ReturnErrorOnFailure(access->Read(path, encoder));
        VerifyOrReturnError(encoder.TriedEncode(), CHIP_ERROR_INCORRECT_STATE);
        ReturnErrorOnFailure(reports.EndOfAttributeReportIBs());
        ReturnErrorOnFailure(writer.EndContainer(outer));
        return writer.Finalize();
    }

    void verify_reads()
    {
        for (auto id : {epm::Attributes::FeatureMap::Id, epm::Attributes::PowerMode::Id,
                        epm::Attributes::NumberOfMeasurementTypes::Id, epm::Attributes::Accuracy::Id,
                        epm::Attributes::Voltage::Id, epm::Attributes::ActiveCurrent::Id,
                        epm::Attributes::ActivePower::Id}) {
            const auto error = encode_read(id);
            if (error != CHIP_NO_ERROR) {
                ESP_LOGE(TAG, "POWER %s read check failed attr=0x%lx: %s",
                         profile_.name, static_cast<unsigned long>(id), error.AsString());
                abort();
            }
        }
        ESP_LOGI(TAG, "POWER %s Matter read/encoding checks passed", profile_.name);
    }

    void log() const
    {
        char voltage[32], current[32], power[32];
        auto format = [](char *buffer, const std::optional<int64_t> &value) {
            if (value) std::snprintf(buffer, 32, "%.3f", *value / 1000.0);
            else std::strcpy(buffer, "null");
        };
        format(voltage, voltage_); format(current, current_); format(power, power_);
        ESP_LOGI(TAG, "POWER %s endpoint=%u mode=%s voltage=%s V current=%s A power=%s W (Modbus)",
            profile_.name, profile_.endpoint, profile_.ac ? "AC" : "DC",
            voltage, current, power);
    }
    Profile profile_;
    std::optional<int64_t> voltage_;
    std::optional<int64_t> current_;
    std::optional<int64_t> power_;
    bool verified_live_ = false;
    epm::Structs::MeasurementAccuracyRangeStruct::Type ranges_[3];
    epm::Structs::MeasurementAccuracyStruct::Type accuracy_[3];
    std::unique_ptr<epm::Instance> instance_;
};

Channel channels[] = {
    Channel({"PV1 Power", "PV1Pwr", 7, false, deye::pv1VoltageV, deye::pv1CurrentA, deye::pv1PowerW}),
    Channel({"PV2 Power", "PV2Pwr", 8, false, deye::pv2VoltageV, deye::pv2CurrentA, deye::pv2PowerW}),
    Channel({"Grid Power", "GridPwr", 9, true, deye::gridVoltageV, deye::gridCurrentA, deye::gridPowerW}),
    Channel({"Battery Power", "BatPwr", 10, false, deye::batteryVoltageV, deye::batteryCurrentA, deye::batteryPowerW}),
    Channel({"Load Power", "LoadPwr", 11, true, deye::loadVoltageV, deye::loadCurrentA, deye::loadPowerW}),
};
esp_matter::endpoint_t *retired_slot = nullptr;
} // namespace

void create(esp_matter::node_t *node, esp_matter::endpoint_t *bridge,
            const uint8_t (&mac)[6], AttachToBridge attach)
{
    // The public SDK API allocates sequential IDs. Consume the retired Power
    // Source slot with a temporary, valid utility endpoint; remove it under the
    // stack lock immediately after start. No new sensor reuses identity 6/SOC.
    esp_matter::endpoint::aggregator::config_t reserved{};
    retired_slot = esp_matter::endpoint::aggregator::create(node, &reserved, esp_matter::ENDPOINT_FLAG_DESTROYABLE, nullptr);
    if (!retired_slot || esp_matter::endpoint::get_id(retired_slot) != 6) {
        ESP_LOGE(TAG, "Cannot reserve retired endpoint 6");
        abort();
    }
    for (auto &channel : channels) channel.create(node, bridge, mac, attach);
}

void start(esp_matter::node_t *node)
{
    ESP_ERROR_CHECK(esp_matter::endpoint::destroy(node, retired_slot));
    retired_slot = nullptr;
    for (auto &channel : channels) channel.start();
}

void update(const inverter_data::Snapshot &snapshot)
{
    for (auto &channel : channels) channel.update(snapshot);
}
} // namespace electrical_measurements
