#pragma once

// Button — non-blocking button input with press and hold detection.
//
// A "press" is defined as a release within 300ms of the button going down.
// A "hold" is defined as the button remaining held beyond 300ms.
//
// Hold thresholds (cumulative time held):
//   300ms  — hold begins, progress bar starts filling
//   5000ms — config mode threshold: "Release to Enter/Exit Wireless Config!"
//   8000ms — factory reset countdown begins (15-segment bar)
//   23000ms — factory reset triggers automatically
//
// All timing is non-blocking. Call update() on every loop() iteration.
// Register callbacks for each event — they fire once at the right moment.

#include <Arduino.h>
#include <functional>

class Button {
public:
    // Callbacks — set before calling begin()
    std::function<void()> onPress;        // Button released within 300ms
    std::function<void(uint32_t)> onHold; // Called every update() while held past 300ms,
                                          // with ms held as argument — use for progress bars
    std::function<void()> onHoldCancelled;     // Released during hold phase before config threshold
    std::function<void()> onConfigThreshold;   // Held to 5s and released
    std::function<void()> onFactoryReset;      // Held to 23s (fires automatically)

    // pin    — GPIO pin the button is wired to (other leg to GND)
    // pullup — true to enable internal pull-up (button reads LOW when pressed)
    Button(uint8_t pin, bool pullup = true);

    // Call once in setup()
    void begin();

    // Call on every loop() iteration — drives all timing and fires callbacks
    void update();

    // Returns true if the button is currently held past the press threshold
    bool isHeld() const;

    // Returns how long the button has been held in ms (0 if not held)
    uint32_t heldMs() const;

private:
    uint8_t  pin_;
    bool     pullup_;
    bool     lastRaw_       = false;  // Raw pin state last update
    bool     pressed_       = false;  // True while button is physically down
    bool     holdFired_     = false;  // True once hold phase has begun
    bool     configFired_   = false;  // True once config threshold callback has fired
    bool     resetFired_    = false;  // True once factory reset callback has fired
    uint32_t pressTime_     = 0;      // millis() when button went down
    uint32_t lastDebounce_  = 0;      // millis() of last state change (for debounce)

    static constexpr uint32_t DEBOUNCE_MS      =   20;
    static constexpr uint32_t PRESS_MAX_MS     =  300;   // Max ms for a press (vs hold)
    static constexpr uint32_t CONFIG_MS        = 5000;   // Hold threshold for config mode
    static constexpr uint32_t RESET_START_MS   = 8000;   // Hold threshold to begin reset bar
    static constexpr uint32_t RESET_TRIGGER_MS = 23000;  // Hold threshold to auto-trigger reset
};
