#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>

namespace deye {
struct Group { uint16_t start; uint16_t count; };
struct Metric {
    const char *key;
    uint16_t address;
    bool is_signed;
    int32_t multiplier;
    int32_t offset;
    uint16_t maximum_raw;
};
inline std::optional<int64_t> decode(const Metric &metric, std::optional<uint16_t> raw)
{
    if (!raw || *raw > metric.maximum_raw || (!metric.is_signed && *raw == 0xffff)) return std::nullopt;
    int64_t word = *raw;
    if (metric.is_signed && word >= 32768) word -= 65536;
    return word * metric.multiplier + metric.offset;
}
inline uint16_t be16(const uint8_t *data) { return (static_cast<uint16_t>(data[0]) << 8) | data[1]; }
inline void put16(uint8_t *data, uint16_t value) { data[0] = value >> 8; data[1] = value & 255; }
inline void request(uint8_t (&data)[12], uint16_t transaction, uint8_t unit, Group group)
{
    put16(data, transaction); put16(data + 2, 0); put16(data + 4, 6);
    data[6] = unit; data[7] = 3;
    put16(data + 8, group.start); put16(data + 10, group.count);
}
// MBAP header is validated before reading the bounded PDU body.
inline bool valid_header(const uint8_t *data, uint16_t transaction, uint8_t unit)
{
    return be16(data) == transaction && be16(data + 2) == 0 && data[6] == unit &&
           be16(data + 4) >= 3 && be16(data + 4) <= 254;
}
inline bool valid_response(const uint8_t *data, size_t size, uint16_t count)
{
    return count >= 1 && count <= 125 && size == static_cast<size_t>(9 + count * 2) &&
           be16(data + 4) == 3 + count * 2 && data[7] == 3 && data[8] == count * 2;
}
} // namespace deye
