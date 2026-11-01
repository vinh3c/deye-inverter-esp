#pragma once

#include <cstdint>
#include <esp_matter.h>
#include "inverter_data.h"

namespace electrical_measurements {
using AttachToBridge = void (*)(esp_matter::endpoint_t *, esp_matter::endpoint_t *, char *,
                               const uint8_t (&)[6], const char *, const char *);

void create(esp_matter::node_t *node, esp_matter::endpoint_t *bridge,
            const uint8_t (&mac)[6], AttachToBridge attach);
// Call under the ChipStack lock after esp_matter::start().
void start(esp_matter::node_t *node);
// Call on the Matter thread with the completed Modbus cycle.
void update(const inverter_data::Snapshot &snapshot);
} // namespace electrical_measurements
