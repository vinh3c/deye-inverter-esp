#include "sonoff_relay.h"
#include "button_debounce.h"

#include <atomic>
#include <app/server/CommissioningWindowManager.h>
#include <app/server/OnboardingCodesUtil.h>
#include <app/server/Server.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <platform/CHIPDeviceLayer.h>

namespace sonoff_relay {
namespace {
constexpr char TAG[] = "sonoff_relay";
constexpr gpio_num_t relay_pin = GPIO_NUM_4;   // Active high.
constexpr gpio_num_t button_pin = GPIO_NUM_9;  // Active low, also the boot strap.
std::atomic<bool> pending{false};
bool ready = false; // Accessed only on the Matter thread or under the stack lock.
ButtonDebounce button(false, 0);
esp_timer_handle_t button_timer = nullptr;
uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void handle_button(intptr_t context)
{
    if (static_cast<ButtonAction>(context) == ButtonAction::Pair) {
        auto &server = chip::Server::GetInstance();
        auto &window = server.GetCommissioningWindowManager();
        const bool commissioned = server.GetFabricTable().FabricCount() != 0;
        if (window.IsCommissioningWindowOpen()) {
            ESP_LOGI(TAG, "PAIR window already open; retaining current session");
        } else {
            const auto error = window.OpenBasicCommissioningWindow(chip::System::Clock::Seconds32(600),
                commissioned ? chip::CommissioningWindowAdvertisement::kDnssdOnly :
                               chip::CommissioningWindowAdvertisement::kAllSupported);
            if (error == CHIP_NO_ERROR) {
                ESP_LOGI(TAG, "PAIR opened for 600 seconds via %s; Wi-Fi/fabrics retained",
                         commissioned ? "network" : "BLE/network");
                PrintOnboardingCodes(chip::RendezvousInformationFlags(commissioned ?
                    chip::RendezvousInformationFlag::kOnNetwork : chip::RendezvousInformationFlag::kBLE));
            } else {
                ESP_LOGE(TAG, "Cannot open PAIR window: %s", error.AsString());
            }
        }
        pending = false;
        return;
    }
    using namespace chip::app::Clusters;
    auto *attribute = esp_matter::attribute::get(endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id);
    auto value = esp_matter_bool(false);
    esp_err_t error = attribute ? esp_matter::attribute::get_val(attribute, &value) : ESP_ERR_NOT_FOUND;
    if (error == ESP_OK) {
        value = esp_matter_bool(!value.val.b);
        error = esp_matter::attribute::update(endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &value);
    }
    if (error != ESP_OK) ESP_LOGE(TAG, "Button toggle failed: %s", esp_err_to_name(error));
    pending = false;
}

void poll_button(void *)
{
    const auto action = button.update(gpio_get_level(button_pin) == 0, now_ms());
    if (action != ButtonAction::None && !pending.exchange(true)) {
        const auto error = chip::DeviceLayer::PlatformMgr().ScheduleWork(handle_button, static_cast<intptr_t>(action));
        if (error != CHIP_NO_ERROR) {
            pending = false;
            ESP_LOGE(TAG, "Cannot schedule button: %s", error.AsString());
        }
    }
}
} // namespace

void init()
{
    // Set the output latch before enabling the driver to avoid an ON pulse.
    ESP_ERROR_CHECK(gpio_set_level(relay_pin, 0));
    gpio_config_t relay{};
    relay.pin_bit_mask = 1ULL << relay_pin;
    relay.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&relay));
    gpio_config_t button{};
    button.pin_bit_mask = 1ULL << button_pin;
    button.mode = GPIO_MODE_INPUT;
    button.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&button));
}

esp_matter::endpoint_t *create(esp_matter::node_t *node)
{
    esp_matter::endpoint::on_off_plugin_unit::config_t config{};
    config.on_off.on_off = false;
    auto *endpoint = esp_matter::endpoint::on_off_plugin_unit::create(
        node, &config, esp_matter::ENDPOINT_FLAG_NONE, nullptr);
    VerifyOrDie(endpoint && esp_matter::endpoint::get_id(endpoint) == endpoint_id);
    return endpoint;
}

esp_err_t attribute_update(esp_matter::attribute::callback_type_t type, uint16_t endpoint,
                           uint32_t cluster, uint32_t attribute, esp_matter_attr_val_t *value, void *)
{
    using namespace chip::app::Clusters;
    if (type != esp_matter::attribute::PRE_UPDATE || endpoint != endpoint_id ||
        cluster != OnOff::Id || attribute != OnOff::Attributes::OnOff::Id) return ESP_OK;
    if (!value || value->type != ESP_MATTER_VAL_TYPE_BOOLEAN) return ESP_ERR_INVALID_ARG;
    // SDK startup may restore saved OnOff/StartUpOnOff before start() forces OFF.
    // Keep the physical output OFF throughout that initialization.
    if (!ready) return ESP_OK;
    const auto error = gpio_set_level(relay_pin, value->val.b ? 1 : 0);
    if (error == ESP_OK) ESP_LOGI(TAG, "RELAY endpoint=%u state=%s GPIO4", endpoint, value->val.b ? "ON" : "OFF");
    return error;
}

void start()
{
    using namespace chip::app::Clusters;
    // Force an explicit, consistent power-on state even if NVS held an old OnOff value.
    ready = true;
    auto off = esp_matter_bool(false);
    ESP_ERROR_CHECK(esp_matter::attribute::update(endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &off));
    button = ButtonDebounce(gpio_get_level(button_pin) == 0, now_ms());
    esp_timer_create_args_t timer{};
    timer.callback = poll_button;
    timer.name = "relay_button";
    timer.dispatch_method = ESP_TIMER_TASK;
    ESP_ERROR_CHECK(esp_timer_create(&timer, &button_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(button_timer, 10000));
    ESP_LOGI(TAG, "Button GPIO9: short release=toggle, hold 5s=pair, debounce=40ms; relay GPIO4 boot=OFF");
}
} // namespace sonoff_relay
