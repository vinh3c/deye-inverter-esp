#include "electrical_measurements.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <esp_log.h>
#include <esp_random.h>
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
    int64_t minimum_mv;
    int64_t maximum_mv;
    int64_t minimum_ma;
    int64_t maximum_ma;
    int64_t initial_mv;
    int64_t initial_ma;
    bool grid_dependent = false;
};

class Channel final : public epm::Delegate {
public:
    explicit Channel(Profile profile) : profile_(profile), voltage_(profile.initial_mv), current_(profile.initial_ma)
    {
        // Accuracy describes synthetic values, not the accuracy of real hardware.
        const int64_t minima[] = {0, profile.minimum_ma, profile.maximum_mv * profile.minimum_ma / 1000};
        const int64_t maxima[] = {profile.maximum_mv, profile.maximum_ma, profile.maximum_mv * profile.maximum_ma / 1000};
        const epm::MeasurementTypeEnum types[] = {
            epm::MeasurementTypeEnum::kVoltage,
            epm::MeasurementTypeEnum::kActiveCurrent,
            epm::MeasurementTypeEnum::kActivePower,
        };
        for (size_t i = 0; i < 3; ++i) {
            ranges_[i].rangeMin = minima[i];
            ranges_[i].rangeMax = maxima[i];
            ranges_[i].fixedMax.SetValue(1);
            accuracy_[i].measurementType = types[i];
            accuracy_[i].measured = false;
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
            !cluster::electrical_power_measurement::attribute::create_voltage(power, voltage_) ||
            !cluster::electrical_power_measurement::attribute::create_active_current(power, current_)) {
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
        log();
    }

    void update(bool grid_connected)
    {
        if (profile_.grid_dependent && !grid_connected) {
            voltage_ = 0;
            current_ = 0;
        } else {
            if (voltage_ == 0) {
                voltage_ = profile_.initial_mv;
                current_ = profile_.initial_ma;
            }
            const int64_t voltage_step = (static_cast<int>(esp_random() % 11) - 5) * 100;
            const int64_t current_step = (static_cast<int>(esp_random() % 21) - 10) * 100;
            voltage_ = std::clamp(voltage_ + voltage_step, profile_.minimum_mv, profile_.maximum_mv);
            current_ = std::clamp(current_ + current_step, profile_.minimum_ma, profile_.maximum_ma);
        }
        // Delegate-backed values must dirty the reporting engine explicitly.
        for (auto id : {epm::Attributes::Voltage::Id, epm::Attributes::ActiveCurrent::Id, epm::Attributes::ActivePower::Id}) {
            MatterReportingAttributeChangeCallback(profile_.endpoint, epm::Id, id);
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
    Reading GetVoltage() override { return dm::MakeNullable(voltage_); }
    Reading GetActiveCurrent() override { return dm::MakeNullable(current_); }
    Reading GetActivePower() override { return dm::MakeNullable(voltage_ * current_ / 1000); }
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
        // the simulated getters. This catches incorrect optional masks or lists.
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
        ESP_LOGI(TAG, "POWER %s endpoint=%u mode=%s voltage=%.3f V current=%.3f A power=%.3f W (simulated)",
            profile_.name, profile_.endpoint, profile_.ac ? "AC" : "DC",
            voltage_ / 1000.0, current_ / 1000.0, (voltage_ * current_ / 1000) / 1000.0);
    }
    Profile profile_;
    int64_t voltage_;
    int64_t current_;
    epm::Structs::MeasurementAccuracyRangeStruct::Type ranges_[3];
    epm::Structs::MeasurementAccuracyStruct::Type accuracy_[3];
    std::unique_ptr<epm::Instance> instance_;
};

Channel channels[] = {
    Channel({"PV1 Power", "PV1Pwr", 7, false, 250000, 450000, 0, 12000, 320000, 5000}),
    Channel({"PV2 Power", "PV2Pwr", 8, false, 250000, 450000, 0, 12000, 340000, 4000}),
    Channel({"Grid Power", "GridPwr", 9, true, 220000, 240000, -10000, 10000, 230000, 3500, true}),
    Channel({"Battery Power", "BatPwr", 10, false, 48000, 56000, -30000, 30000, 51200, -5000}),
    Channel({"Load Power", "LoadPwr", 11, true, 220000, 240000, 0, 20000, 230000, 5000}),
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

void update(bool grid_connected)
{
    for (auto &channel : channels) channel.update(grid_connected);
}
} // namespace electrical_measurements
