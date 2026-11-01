#pragma once
#include <array>
#include <optional>
#include "deye_registers.h"

namespace inverter_data {
struct Snapshot {
    std::array<std::optional<uint16_t>, deye::METRIC_COUNT> raw{};
    std::optional<int64_t> get(deye::Index index) const { return deye::decode(deye::METRICS[index], raw[index]); }
};
// Worker performs blocking TCP reads away from the Matter thread, then hands off
// ownership of a complete cycle to the Matter callback (including partial errors).
using Callback = void (*)(intptr_t);
void start(Callback callback);
void publication_complete();
} // namespace inverter_data
