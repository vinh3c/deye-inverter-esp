#include "device_identity.h"

#include <cstdio>
#include <cstring>

#include <esp_log.h>
#include <platform/DeviceInstanceInfoProvider.h>

namespace device_identity {
namespace {
CHIP_ERROR copy_string(char *buffer, size_t size, const char *value)
{
    if (size <= std::strlen(value)) {
        return CHIP_ERROR_BUFFER_TOO_SMALL;
    }
    std::strcpy(buffer, value);
    return CHIP_NO_ERROR;
}

// Keep the SDK's VID/PID, hardware version and other factory-data behavior.
// Override only the three human-readable identity fields requested by the app.
class ProjectDeviceInfo final : public chip::DeviceLayer::DeviceInstanceInfoProvider {
public:
    explicit ProjectDeviceInfo(chip::DeviceLayer::DeviceInstanceInfoProvider *fallback) : fallback_(fallback) {}
    char serial[33]{};

    CHIP_ERROR GetVendorName(char *buffer, size_t size) override { return copy_string(buffer, size, manufacturer); }
    CHIP_ERROR GetProductName(char *buffer, size_t size) override { return copy_string(buffer, size, model); }
    CHIP_ERROR GetSerialNumber(char *buffer, size_t size) override { return copy_string(buffer, size, serial); }
    CHIP_ERROR GetVendorId(uint16_t &value) override { return fallback_->GetVendorId(value); }
    CHIP_ERROR GetProductId(uint16_t &value) override { return fallback_->GetProductId(value); }
    CHIP_ERROR GetPartNumber(char *buffer, size_t size) override { return fallback_->GetPartNumber(buffer, size); }
    CHIP_ERROR GetProductURL(char *buffer, size_t size) override { return fallback_->GetProductURL(buffer, size); }
    CHIP_ERROR GetProductLabel(char *buffer, size_t size) override { return fallback_->GetProductLabel(buffer, size); }
    CHIP_ERROR GetManufacturingDate(uint16_t &year, uint8_t &month, uint8_t &day) override
    {
        return fallback_->GetManufacturingDate(year, month, day);
    }
    CHIP_ERROR GetHardwareVersion(uint16_t &value) override { return fallback_->GetHardwareVersion(value); }
    CHIP_ERROR GetHardwareVersionString(char *buffer, size_t size) override
    {
        return fallback_->GetHardwareVersionString(buffer, size);
    }
    CHIP_ERROR GetRotatingDeviceIdUniqueId(chip::MutableByteSpan &value) override
    {
        return fallback_->GetRotatingDeviceIdUniqueId(value);
    }
    CHIP_ERROR GetProductFinish(chip::app::Clusters::BasicInformation::ProductFinishEnum *value) override
    {
        return fallback_->GetProductFinish(value);
    }
    CHIP_ERROR GetProductPrimaryColor(chip::app::Clusters::BasicInformation::ColorEnum *value) override
    {
        return fallback_->GetProductPrimaryColor(value);
    }

private:
    chip::DeviceLayer::DeviceInstanceInfoProvider *fallback_;
};
} // namespace

void serial_number(char *buffer, size_t size, const uint8_t (&mac)[6], const char *key)
{
    if (!key) {
        std::snprintf(buffer, size, "%s-%02X%02X%02X%02X%02X%02X", model,
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return;
    }
    std::snprintf(buffer, size, "%.10s-%02X%02X%02X%02X%02X%02X%s%.7s", model,
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], key ? "-" : "", key ? key : "");
}

void install_provider(const uint8_t (&mac)[6])
{
    static ProjectDeviceInfo provider(chip::DeviceLayer::GetDeviceInstanceInfoProvider());
    serial_number(provider.serial, sizeof(provider.serial), mac);
    chip::DeviceLayer::SetDeviceInstanceInfoProvider(&provider);
    ESP_LOGI("deye_sensors", "DEVICE manufacturer=%s model=%s serial=%s", manufacturer, model, provider.serial);
}
} // namespace device_identity
