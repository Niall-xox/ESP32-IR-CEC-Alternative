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
    if ((millis() - lastDebounce_) < HoldTimings::DEBOUNCE_MS) return;

    // --- Falling edge: button just went down ---
    if (raw && !pressed_) {
        pressed_    = true;
        resetFired_ = false;
        pressTime_  = millis();
        return;
    }

    // --- Rising edge: button just released ---
    if (!raw && pressed_) {
        pressed_        = false;
        uint32_t held   = millis() - pressTime_;

        if (held < HoldTimings::PRESS_MAX_MS) {
            // Released quickly — treat as a press
            if (onPress) onPress();
        } else if (held < HoldTimings::CONFIG_MS) {
            // Released during hold bar phase (300ms–5s) — cancel
            if (onHoldCancelled) onHoldCancelled();
        } else if (!resetFired_) {
            if (held < HoldTimings::RESET_START_MS) {
                // Released in the 5s–8s window ("Release to Enter/Exit" screen) —
                // trigger config mode toggle
                if (onConfigThreshold) onConfigThreshold();
            } else {
                // Released during factory reset bar (8s–23s) — cancel
                if (onHoldCancelled) onHoldCancelled();
            }
        }
        // held >= RESET_TRIGGER_MS and resetFired_: factory reset already
        // triggered automatically — nothing to do on release
        return;
    }

    // --- While held ---
    if (pressed_) {
        uint32_t held = millis() - pressTime_;

        // Once past the press threshold, begin the hold phase
        if (held >= HoldTimings::PRESS_MAX_MS) {
            // Fire onHold every update() so the display can draw the progress bar
            if (onHold) onHold(held);

            // The config threshold deliberately has no callback here — entering
            // config mode fires on release instead, so the user sees "Release To
            // Enter Wireless Config!" and acts on it. The release handler works
            // out what to do from the elapsed hold time alone.

            // Factory reset: auto-triggers at 23s, no release needed
            if (held >= HoldTimings::RESET_END_MS && !resetFired_) {
                resetFired_ = true;
                if (onFactoryReset) onFactoryReset();
            }
        }
    }
}
