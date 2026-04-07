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
                break;
            case TimerAction::ShowStatus:
                drawStatus(lastProfile_, daemonStatus_);
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// State setters
// ---------------------------------------------------------------------------

void Display::showStatus(const String& profileName, DaemonStatus status, bool alwaysOn) {
    if (!ok_) return;
    daemonStatus_ = status;
    lastProfile_  = profileName;
    lastAlwaysOn_ = alwaysOn;

    drawStatus(profileName, status);

    if (!alwaysOn) {
        // Start timer — display turns off after STATUS_TIMEOUT_MS
        timerActive_   = true;
        timerStart_    = millis();
        timerDuration_ = STATUS_TIMEOUT_MS;
        timerAction_   = TimerAction::TurnOff;
    } else {
        timerActive_ = false;  // Always-on: no timer
    }
}

void Display::showIRConfirm(bool on, bool alwaysOn, const String& profileName) {
    if (!ok_) return;
    lastProfile_  = profileName;
    lastAlwaysOn_ = alwaysOn;

    oled_.clearDisplay();
    oled_.setTextSize(2);
    oled_.setTextColor(SSD1306_WHITE);
    oled_.setCursor(0, 8);
    oled_.print(on ? "TV On" : "TV Off");
    oled_.display();

    timerActive_   = true;
    timerStart_    = millis();
    timerDuration_ = IR_CONFIRM_TIMEOUT_MS;
    // After confirmation: turn off (button-press will update status if needed)
    timerAction_   = alwaysOn ? TimerAction::ShowStatus : TimerAction::TurnOff;
}

void Display::showHoldBar(uint32_t heldMs, bool enteringWifi,
                          bool alwaysOn, const String& profileName) {
    if (!ok_) return;
    timerActive_  = false;  // Cancel any pending timer while bar is active
    lastProfile_  = profileName;
    lastAlwaysOn_ = alwaysOn;

    oled_.clearDisplay();
    oled_.setTextSize(1);
    oled_.setTextColor(SSD1306_WHITE);

    // Top line — instruction text
    oled_.setCursor(0, 0);
    oled_.print(enteringWifi ? "Enter Wireless Config?" : "Exit Wireless Config?");

    // Progress bar — 5 blocks over 5 seconds (300ms–5000ms hold range)
    // Map held time from [PRESS_MAX_MS, CONFIG_MS] → [0, 5] filled blocks
    uint32_t holdRange = CONFIG_MS - 300;  // 4700ms range
    uint32_t elapsed   = heldMs > 300 ? heldMs - 300 : 0;
    uint8_t  filled    = (uint8_t)((elapsed * 5) / holdRange);
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

    // 15-segment bar over the 8000ms–23000ms hold range
    uint32_t elapsed = heldMs > RESET_START_MS ? heldMs - RESET_START_MS : 0;
    uint32_t range   = RESET_END_MS - RESET_START_MS;  // 15000ms
    uint8_t  filled  = (uint8_t)((elapsed * 15) / range);
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

void Display::setDaemonStatus(DaemonStatus status) {
    daemonStatus_ = status;
    // If the display is currently showing the status screen, redraw it
    // so the daemon line updates immediately without waiting for a button press.
    if (!timerActive_ || timerAction_ == TimerAction::ShowStatus) {
        drawStatus(lastProfile_, daemonStatus_);
    }
}

// ---------------------------------------------------------------------------
// Internal draw helpers
// ---------------------------------------------------------------------------

void Display::drawStatus(const String& profileName, DaemonStatus status) {
    if (!ok_) return;
    oled_.clearDisplay();
    oled_.setTextSize(1);
    oled_.setTextColor(SSD1306_WHITE);

    // Line 1 — active profile name
    oled_.setCursor(0, 0);
    oled_.print("Profile: ");
    oled_.print(profileName);

    // Line 2 — daemon connection status
    oled_.setCursor(0, 12);
    oled_.print("Daemon: ");
    oled_.print(daemonStatusStr(status));

    oled_.display();
}

// Draw a block-style progress bar.
// filled — number of filled blocks, total — total number of blocks, y — top pixel row.
void Display::drawProgressBar(uint8_t filled, uint8_t total, uint8_t y) {
    // Each block is sized to fit the 128px width with 1px gaps between blocks
    // Block width = (128 - (total - 1)) / total, rounded down
    uint8_t blockW = (128 - (total - 1)) / total;
    uint8_t barH   = 10;

    for (uint8_t i = 0; i < total; i++) {
        uint8_t x = i * (blockW + 1);
        if (i < filled) {
            // Filled block — solid rectangle
            oled_.fillRect(x, y, blockW, barH, SSD1306_WHITE);
        } else {
            // Empty block — outline only
            oled_.drawRect(x, y, blockW, barH, SSD1306_WHITE);
        }
    }
}

const char* Display::daemonStatusStr(DaemonStatus s) {
    switch (s) {
        case DaemonStatus::Waiting:   return "Waiting...";
        case DaemonStatus::Connected: return "Connected";
        case DaemonStatus::NotFound:  return "Not Found";
        default:                      return "Unknown";
    }
}
