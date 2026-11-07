#pragma once

#include <esp_matter.h>

namespace sonoff_relay {
constexpr uint16_t endpoint_id = 12;
void init();
esp_matter::endpoint_t *create(esp_matter::node_t *node);
esp_err_t attribute_update(esp_matter::attribute::callback_type_t type, uint16_t endpoint,
                           uint32_t cluster, uint32_t attribute, esp_matter_attr_val_t *value, void *);
// Called after esp_matter::start, under the ChipStack lock.
void start();
} // namespace sonoff_relay
