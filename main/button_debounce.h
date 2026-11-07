#pragma once

#include <cstdint>

namespace sonoff_relay {
enum class ButtonAction { None, Toggle, Pair };

// Short presses act on release; long presses fire once without toggling the relay.
// A button held at boot must first be released.
class ButtonDebounce {
public:
    explicit ButtonDebounce(bool pressed, uint32_t now) : raw_(pressed), stable_(pressed), changed_(now) {}
    ButtonAction update(bool pressed, uint32_t now)
    {
        if (pressed != raw_) {
            raw_ = pressed;
            changed_ = now;
        }
        if (raw_ != stable_ && uint32_t(now - changed_) >= 40) {
            stable_ = raw_;
            if (stable_) {
                tracking_ = true;
                long_sent_ = false;
                pressed_at_ = changed_;
            } else if (tracking_) {
                tracking_ = false;
                if (!long_sent_) {
                    return uint32_t(changed_ - pressed_at_) >= 5000 ? ButtonAction::Pair : ButtonAction::Toggle;
                }
            }
        }
        if (tracking_ && stable_ && raw_ && !long_sent_ && uint32_t(now - pressed_at_) >= 5000) {
            long_sent_ = true;
            return ButtonAction::Pair;
        }
        return ButtonAction::None;
    }
private:
    bool raw_;
    bool stable_;
    uint32_t changed_;
    uint32_t pressed_at_ = 0;
    bool tracking_ = false;
    bool long_sent_ = false;
};
} // namespace sonoff_relay
