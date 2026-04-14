#include "Display.h"

Display::Display(Adafruit_SSD1306& oled) : oled_(oled) {}

void Display::begin() {
    ok_ = true;
    off();
}

void Display::update() {
    if (!ok_ || !timerActive_) return;

    if ((millis() - timerStart_) >= timerDuration_) {
        timerActive_ = false;
        switch (timerAction_) {
            case TimerAction::TurnOff:
                off();
                // Notify main so pressCount can be reset
                if (onExpire) onExpire();
                break;
            case TimerAction::ShowStatus:
                drawStatus(lastProfile_);
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// State setters
// ---------------------------------------------------------------------------

void Display::showStatus(const String& profileName, bool alwaysOn) {
    if (!ok_) return;
    lastProfile_ = profileName;

    drawStatus(profileName);

    if (!alwaysOn) {
        timerActive_   = true;
        timerStart_    = millis();
        timerDuration_ = STATUS_TIMEOUT_MS;
        timerAction_   = TimerAction::TurnOff;
    } else {
        timerActive_ = false;
    }
}

void Display::showIRConfirm(bool on, bool alwaysOn, const String& profileName) {
    if (!ok_) return;
    lastProfile_ = profileName;

    oled_.clearDisplay();
    oled_.setTextSize(2);
    oled_.setTextColor(SSD1306_WHITE);
    oled_.setCursor(0, 8);
    oled_.print(on ? "TV On" : "TV Off");
    oled_.display();

    timerActive_   = true;
    timerStart_    = millis();
    timerDuration_ = IR_CONFIRM_TIMEOUT_MS;
    timerAction_   = alwaysOn ? TimerAction::ShowStatus : TimerAction::TurnOff;
}

void Display::showHoldBar(uint32_t heldMs, bool enteringWifi) {
    if (!ok_) return;
    timerActive_ = false;

    oled_.clearDisplay();
    oled_.setTextSize(1);
    oled_.setTextColor(SSD1306_WHITE);

    oled_.setCursor(0, 0);
    oled_.print(enteringWifi ? "Enter Wireless Config?" : "Exit Wireless Config?");

    // 5-block bar filling from 300ms to 5000ms
    uint32_t elapsed = heldMs > 300 ? heldMs - 300 : 0;
    uint8_t  filled  = (uint8_t)((elapsed * 5) / (CONFIG_MS - 300));
    if (filled > 5) filled = 5;
    drawProgressBar(filled, 5, 20);

    oled_.display();
}

void Display::showResetBar(uint32_t heldMs) {
    if (!ok_) return;
    timerActive_ = false;

    oled_.clearDisplay();
    oled_.setTextSize(1);
    oled_.setTextColor(SSD1306_WHITE);

    oled_.setCursor(0, 0);
    oled_.print("Hold To Factory Reset");

    // 15-segment bar filling from 8s to 23s
    uint32_t elapsed = heldMs > RESET_START_MS ? heldMs - RESET_START_MS : 0;
    uint8_t  filled  = (uint8_t)((elapsed * 15) / (RESET_END_MS - RESET_START_MS));
    if (filled > 15) filled = 15;
    drawProgressBar(filled, 15, 20);

    oled_.display();
}

void Display::showConfigRelease(bool entering) {
    if (!ok_) return;
    timerActive_ = false;

    oled_.clearDisplay();
    oled_.setTextSize(1);
    oled_.setTextColor(SSD1306_WHITE);
    oled_.setCursor(0, 0);
    oled_.print(entering ? "Release To Enter" : "Release To Exit");
    oled_.setCursor(0, 12);
    oled_.print("Wireless Config!");
    oled_.display();
}

void Display::showWifiLockMessage() {
    if (!ok_) return;

    oled_.clearDisplay();
    oled_.setTextSize(1);
    oled_.setTextColor(SSD1306_WHITE);
    oled_.setCursor(0, 0);
    oled_.print("Hold Button to Exit");
    oled_.setCursor(0, 11);
    oled_.print("Wireless Mode to");
    oled_.setCursor(0, 22);
    oled_.print("Switch Profiles");
    oled_.display();

    timerActive_   = true;
    timerStart_    = millis();
    timerDuration_ = WIFI_MSG_TIMEOUT_MS;
    timerAction_   = TimerAction::ShowStatus;
}

void Display::off() {
    if (!ok_) return;
    timerActive_ = false;
    oled_.clearDisplay();
    oled_.display();
}

void Display::setWifiActive(bool active) {
    wifiActive_ = active;
}

void Display::setWifiIP(const String& ip) {
    wifiIP_ = ip;
}

// ---------------------------------------------------------------------------
// Internal draw helpers
// ---------------------------------------------------------------------------

void Display::drawStatus(const String& profileName) {
    if (!ok_) return;
    oled_.clearDisplay();
    oled_.setTextSize(1);
    oled_.setTextColor(SSD1306_WHITE);

    if (wifiActive_) {
        // Three-line layout: profile / wifi status / hostname
        oled_.setCursor(0, 0);
        oled_.print("Profile: ");
        oled_.print(profileName);

        oled_.setCursor(0, 11);
        oled_.print("WiFi: Active");

        oled_.setCursor(0, 22);
        oled_.print(wifiIP_);
    } else {
        // Single line: profile name, vertically centered
        oled_.setCursor(0, 12);
        oled_.print("Profile: ");
        oled_.print(profileName);
    }

    oled_.display();
}

void Display::drawProgressBar(uint8_t filled, uint8_t total, uint8_t y) {
    uint8_t blockW = (128 - (total - 1)) / total;
    uint8_t barH   = 10;

    for (uint8_t i = 0; i < total; i++) {
        uint8_t x = i * (blockW + 1);
        if (i < filled) {
            oled_.fillRect(x, y, blockW, barH, SSD1306_WHITE);
        } else {
            oled_.drawRect(x, y, blockW, barH, SSD1306_WHITE);
        }
    }
}
