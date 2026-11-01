#include <cstdlib>

#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_console.h>
#include <nvs_flash.h>
#include <app/server/CommissioningWindowManager.h>
#include <app/server/OnboardingCodesUtil.h>
#include <app/server/Server.h>

namespace {
constexpr char TAG[] = "matter_switch";
uint16_t switch_endpoint = 0;

esp_err_t on_attribute(esp_matter::attribute::callback_type_t type, uint16_t endpoint,
                       uint32_t cluster, uint32_t attribute, esp_matter_attr_val_t *value, void *)
{
    using namespace chip::app::Clusters;
    if (type == esp_matter::attribute::POST_UPDATE && endpoint == switch_endpoint &&
        cluster == OnOff::Id && attribute == OnOff::Attributes::OnOff::Id) {
        ESP_LOGI(TAG, "SWITCH endpoint=%u state=%s (simulated)", endpoint, value->val.b ? "ON" : "OFF");
    }
    return ESP_OK;
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
        ESP_LOGI(TAG, "PAIR complete; switch is ready");
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
    auto *node = esp_matter::node::create(&node_config, on_attribute, on_identify);
    if (!node) {
        ESP_LOGE(TAG, "Cannot create Matter node");
        abort();
    }

    // A controllable binary load, exposed as an outlet/switch in controller apps.
    esp_matter::endpoint::on_off_plugin_unit::config_t switch_config{};
    switch_config.on_off.on_off = false;
    auto *endpoint = esp_matter::endpoint::on_off_plugin_unit::create(
        node, &switch_config, esp_matter::ENDPOINT_FLAG_NONE, nullptr);
    if (!endpoint) {
        ESP_LOGE(TAG, "Cannot create switch endpoint");
        abort();
    }
    switch_endpoint = esp_matter::endpoint::get_id(endpoint);
    ESP_ERROR_CHECK(esp_matter::start(on_event));

    // Read the actual attribute after Matter restores persisted state.
    esp_matter_attr_val_t state = esp_matter_invalid(nullptr);
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
        PrintOnboardingCodes(chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));
    }
    auto *on_off = esp_matter::attribute::get(switch_endpoint, chip::app::Clusters::OnOff::Id,
        chip::app::Clusters::OnOff::Attributes::OnOff::Id);
    const esp_err_t read_error = esp_matter::attribute::get_val(on_off, &state);
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    ESP_ERROR_CHECK(read_error);
    ESP_LOGI(TAG, "SWITCH endpoint=%u state=%s (simulated, boot)", switch_endpoint, state.val.b ? "ON" : "OFF");

#if CONFIG_ENABLE_CHIP_SHELL
    ESP_ERROR_CHECK(esp_matter::console::diagnostics_register_commands());
    ESP_ERROR_CHECK(esp_matter::console::factoryreset_register_commands());
    ESP_ERROR_CHECK(esp_matter::console::init());
#endif
}
