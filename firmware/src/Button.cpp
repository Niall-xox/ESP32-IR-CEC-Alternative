#include "Button.h"

Button::Button(uint8_t pin, bool pullup)
    : pin_(pin), pullup_(pullup) {}

void Button::begin() {
    pinMode(pin_, pullup_ ? INPUT_PULLUP : INPUT);
}

void Button::update() {
    // --- Debounce ---
    // Read the raw pin. With INPUT_PULLUP, LOW = pressed, HIGH = released.
    bool raw = pullup_ ? (digitalRead(pin_) == LOW) : (digitalRead(pin_) == HIGH);

    if (raw != lastRaw_) {
        lastDebounce_ = millis();
        lastRaw_ = raw;
    }

    // Ignore state changes within the debounce window
    if ((millis() - lastDebounce_) < DEBOUNCE_MS) return;

    // --- Falling edge: button just went down ---
    if (raw && !pressed_) {
        pressed_     = true;
        holdFired_   = false;
        configFired_ = false;
        resetFired_  = false;
        pressTime_   = millis();
        return;
    }

    // --- Rising edge: button just released ---
    if (!raw && pressed_) {
        pressed_        = false;
        uint32_t held   = millis() - pressTime_;

        if (held < PRESS_MAX_MS) {
            // Released quickly — treat as a press
            if (onPress) onPress();
        } else if (held >= CONFIG_MS && !configFired_) {
            // Released at or after the config threshold but before reset countdown
            // configFired_ guards against firing if it was already handled during hold
            if (onConfigThreshold) onConfigThreshold();
            configFired_ = true;
        }
        // If released during hold phase but before config threshold — no action.
        // The progress bar cancels cleanly by simply not completing.
        return;
    }

    // --- While held ---
    if (pressed_) {
        uint32_t held = millis() - pressTime_;

        // Once past the press threshold, begin the hold phase
        if (held >= PRESS_MAX_MS) {
            holdFired_ = true;

            // Fire onHold every update() so the display can draw the progress bar
            if (onHold) onHold(held);

            // Config threshold reached — flag it so release handler knows
            if (held >= CONFIG_MS && !configFired_) {
                configFired_ = true;
                // Don't fire the callback here — fire on release so the user
                // sees "Release to Enter Wireless Config!" and acts deliberately.
                // The release handler checks configFired_ to know what to do.
            }

            // Factory reset: auto-triggers at 23s, no release needed
            if (held >= RESET_TRIGGER_MS && !resetFired_) {
                resetFired_ = true;
                if (onFactoryReset) onFactoryReset();
            }
        }
    }
}

bool Button::isHeld() const {
    return pressed_ && (millis() - pressTime_) >= PRESS_MAX_MS;
}

uint32_t Button::heldMs() const {
    if (!pressed_) return 0;
    return millis() - pressTime_;
}
