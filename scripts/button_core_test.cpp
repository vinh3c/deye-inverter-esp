#include "button_debounce.h"
#include <cassert>
#include <cstdint>

using sonoff_relay::ButtonAction;
using sonoff_relay::ButtonDebounce;
constexpr auto none = ButtonAction::None;
constexpr auto toggle = ButtonAction::Toggle;
constexpr auto pair = ButtonAction::Pair;

int main()
{
    ButtonDebounce button(false, 0);
    assert(button.update(true, 10) == none);
    assert(button.update(false, 20) == none); // Press bounce.
    assert(button.update(true, 30) == none);
    assert(button.update(true, 70) == none); // No toggle before release.
    assert(button.update(false, 100) == none);
    assert(button.update(true, 110) == none); // Release bounce.
    assert(button.update(false, 120) == none);
    assert(button.update(false, 159) == none);
    assert(button.update(false, 160) == toggle);
    assert(button.update(false, 200) == none);

    assert(button.update(true, 300) == none);
    assert(button.update(true, 340) == none);
    assert(button.update(true, 5299) == none);
    assert(button.update(true, 5300) == pair); // Exactly 5 seconds from raw press.
    assert(button.update(true, 10300) == none); // Long hold must not repeat.
    assert(button.update(false, 10310) == none);
    assert(button.update(false, 10350) == none); // No toggle after pairing.

    // Release before 5 seconds stays short despite release debounce.
    assert(button.update(true, 11000) == none);
    assert(button.update(true, 11040) == none);
    assert(button.update(false, 15999) == none);
    assert(button.update(false, 16039) == toggle);

    // A delayed sample after a long release still opens pairing exactly once.
    assert(button.update(true, 17000) == none);
    assert(button.update(true, 17040) == none);
    assert(button.update(false, 22000) == none);
    assert(button.update(false, 22040) == pair);
    assert(button.update(false, 22100) == none);

    ButtonDebounce held_at_boot(true, 0);
    assert(held_at_boot.update(true, 10000) == none);
    assert(held_at_boot.update(false, 10010) == none);
    assert(held_at_boot.update(false, 10050) == none);
    assert(held_at_boot.update(true, 10060) == none);
    assert(held_at_boot.update(true, 10100) == none);
    assert(held_at_boot.update(false, 10200) == none);
    assert(held_at_boot.update(false, 10240) == toggle);

    ButtonDebounce rollover(false, UINT32_MAX - 100);
    assert(rollover.update(true, UINT32_MAX - 60) == none);
    assert(rollover.update(true, UINT32_MAX - 20) == none);
    assert(rollover.update(true, 4938) == none);
    assert(rollover.update(true, 4939) == pair); // 5 seconds across uint32 wrap.
    assert(rollover.update(false, 5000) == none);
    assert(rollover.update(false, 5040) == none);
}
