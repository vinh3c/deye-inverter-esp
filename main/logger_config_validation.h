#pragma once

#include <cstdint>
#include <string_view>

namespace logger_config {
inline bool valid_host(std::string_view host)
{
    const auto alnum = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    };
    if (host.empty() || host.size() > 63 || !alnum(host.front()) || !alnum(host.back())) return false;
    for (char c : host) if (!alnum(c) && c != '-' && c != '.') return false;
    return true;
}

inline bool parse_port(std::string_view text, uint16_t &port)
{
    if (text.empty()) return false;
    uint32_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
        if (value > 65535) return false;
    }
    if (!value) return false;
    port = static_cast<uint16_t>(value);
    return true;
}
} // namespace logger_config
