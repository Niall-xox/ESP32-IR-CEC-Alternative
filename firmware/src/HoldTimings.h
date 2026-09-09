#pragma once

#include <Arduino.h>

namespace HoldTimings {

    static constexpr uint32_t DEBOUNCE_MS    =    20;

    static constexpr uint32_t PRESS_MAX_MS   =   300;

    static constexpr uint32_t CONFIG_MS      =  4000;

    // The window between reaching the wireless-config threshold and the factory
    // reset bar starting — how long you have to let go. Kept at 3s, unchanged
    // from when CONFIG_MS was 5000, because that is the part that has been
    // tested by hand and felt right.
    static constexpr uint32_t RESET_START_MS =  7000;

    static constexpr uint32_t RESET_END_MS   = 16000;

}
