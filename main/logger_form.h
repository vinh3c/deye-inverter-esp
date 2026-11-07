#pragma once

#include "logger_config_validation.h"
#include <cstring>

namespace logger_form {
constexpr size_t max_body = 256;
enum class Result { Ok, Invalid, Forbidden };
struct Values {
    char host[64]{};
    uint16_t port = 0;
};

inline bool decode(std::string_view input, char *output, size_t capacity)
{
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    size_t length = 0;
    for (size_t i = 0; i < input.size(); ++i) {
        unsigned char c = input[i];
        if (c == '%') {
            if (i + 2 >= input.size()) return false;
            const int high = hex(input[i + 1]), low = hex(input[i + 2]);
            if (high < 0 || low < 0) return false;
            c = static_cast<unsigned char>((high << 4) | low);
            i += 2;
        } else if (c == '+') {
            c = ' ';
        }
        if (c <= 32 || c >= 127 || length + 1 >= capacity) return false;
        output[length++] = static_cast<char>(c);
    }
    output[length] = '\0';
    return true;
}

inline Result parse(std::string_view body, std::string_view expected_token, Values &output)
{
    if (body.empty() || body.size() > max_body || body.back() == '&') return Result::Invalid;
    Values values{};
    char token[33]{};
    unsigned seen = 0;
    while (!body.empty()) {
        const size_t separator = body.find('&');
        const auto field = body.substr(0, separator);
        const auto equals = field.find('=');
        if (equals == std::string_view::npos) return Result::Invalid;
        const auto key = field.substr(0, equals), encoded = field.substr(equals + 1);
        const unsigned bit = key == "host" ? 1 : key == "port" ? 2 : key == "token" ? 4 : 0;
        if (!bit || (seen & bit)) return Result::Invalid;
        seen |= bit;
        char value[64]{};
        if (!decode(encoded, value, sizeof(value))) return Result::Invalid;
        if (bit == 1) {
            if (!logger_config::valid_host(value)) return Result::Invalid;
            std::strcpy(values.host, value);
        } else if (bit == 2) {
            if (!logger_config::parse_port(value, values.port)) return Result::Invalid;
        } else {
            if (std::strlen(value) != 32) return Result::Forbidden;
            std::strcpy(token, value);
        }
        if (separator == std::string_view::npos) break;
        body.remove_prefix(separator + 1);
    }
    if (seen != 7) return Result::Invalid;
    if (expected_token.size() != 32 || std::string_view(token) != expected_token) return Result::Forbidden;
    output = values;
    return Result::Ok;
}
} // namespace logger_form
