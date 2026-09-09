#pragma once

#include <Arduino.h>

namespace HoldTimings {

    static constexpr uint32_t DEBOUNCE_MS    =    20;

    static constexpr uint32_t PRESS_MAX_MS   =   300;

    static constexpr uint32_t CONFIG_MS      =  5000;

    static constexpr uint32_t RESET_START_MS =  8000;

    static constexpr uint32_t RESET_END_MS   = 23000;

}
