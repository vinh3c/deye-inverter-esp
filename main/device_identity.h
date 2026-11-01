#pragma once

#include <cstddef>
#include <cstdint>

namespace device_identity {
inline char manufacturer[] = "deye-inverter-esp";
inline char model[] = DEYE_MODEL_NAME;

void serial_number(char *buffer, size_t size, const uint8_t (&mac)[6], const char *key = nullptr);
// Call with the Matter stack locked, after esp_matter::start installs its providers.
void install_provider(const uint8_t (&mac)[6]);
} // namespace device_identity
