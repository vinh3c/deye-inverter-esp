#pragma once

#include <cstddef>
#include <cstring>

namespace logger_web {
struct Replacement { const char *key; const char *value; };

// Emit the template and substitutions without allocating a copy of the page.
// A null value delegates the special notice markup to the caller.
template <typename Emit, typename Notice>
bool stream_template(const char *cursor, const Replacement *replacements, size_t count, Emit emit, Notice notice)
{
    for (;;) {
        const char *next = nullptr;
        const Replacement *replacement = nullptr;
        for (size_t i = 0; i < count; ++i) {
            const char *found = std::strstr(cursor, replacements[i].key);
            if (found && (!next || found < next)) { next = found; replacement = &replacements[i]; }
        }
        if (!next) return !*cursor || emit(cursor, std::strlen(cursor));
        if (next != cursor && !emit(cursor, next - cursor)) return false;
        if (replacement->value) {
            if (*replacement->value && !emit(replacement->value, std::strlen(replacement->value))) return false;
        } else if (!notice()) {
            return false;
        }
        cursor = next + std::strlen(replacement->key);
    }
}
} // namespace logger_web
