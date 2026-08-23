#pragma once

// Button hold thresholds.
//
// Shared by Button, which decides what a hold *means*, and Display, which draws
// progress bars spanning the same ranges. Both previously carried their own
// copies of 5000 / 8000 / 23000; a change to one without the other would have
// left the bar filling over a different window than the callback fires on —
// visibly wrong and easy to miss. One definition makes that impossible.

#include <Arduino.h>

namespace HoldTimings {

    // Ignore state changes within this window of the last transition.
    static constexpr uint32_t DEBOUNCE_MS    =    20;

    // Released before this is a press; held past it begins the hold phase.
    static constexpr uint32_t PRESS_MAX_MS   =   300;

    // Released between CONFIG_MS and RESET_START_MS toggles wireless config mode.
    static constexpr uint32_t CONFIG_MS      =  5000;

    // Held past this, the factory reset countdown replaces the config bar.
    static constexpr uint32_t RESET_START_MS =  8000;

    // Held to this, factory reset triggers automatically — no release needed.
    static constexpr uint32_t RESET_END_MS   = 23000;

}  // namespace HoldTimings
