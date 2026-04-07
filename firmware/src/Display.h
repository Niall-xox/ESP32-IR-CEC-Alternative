#pragma once

// Display — OLED state management for the 128x32 SSD1306.
//
// States:
//   OFF        — blank
//   STATUS     — profile name + daemon status (+ WiFi: Active when in WiFi mode)
//   IR_CONFIRM — "TV On" or "TV Off" for 2 seconds after an IR command
//   HOLD_BAR   — 5-block progress bar while button held (config mode countdown)
//   RESET_BAR  — 15-segment bar when held past config threshold
//   WIFI_MSG   — "Hold Button to Exit Wireless Mode..." on press 2 in WiFi mode
//
// Timed states expire automatically — call update() on every loop() iteration.

#include <Arduino.h>
#include <Adafruit_SSD1306.h>
#include <functional>

enum class DaemonStatus {
    Unknown,    // No PING sent yet
    Waiting,    // PING sent, awaiting PONG
    Connected,  // PONG received
    NotFound    // PING timed out
};

class Display {
public:
    // Fired when the STATUS screen timer expires and the display turns off.
    // Used by main.cpp to reset the press counter.
    std::function<void()> onExpire;

    Display(Adafruit_SSD1306& oled);

    void begin();
    void update();  // Call every loop() — handles timed state expiry

    // --- State setters ---

    // Show status screen. In non-always-on mode starts a 2s timer then turns off.
    void showStatus(const String& profileName, DaemonStatus status, bool alwaysOn);

    // Show "TV On" or "TV Off" for 2s then turn off (or return to status if always-on).
    void showIRConfirm(bool on, bool alwaysOn, const String& profileName);

    // Show the hold progress bar. Called repeatedly by Button::onHold.
    void showHoldBar(uint32_t heldMs, bool enteringWifi, bool alwaysOn, const String& profileName);

    // Show the factory reset countdown bar.
    void showResetBar(uint32_t heldMs);

    // Show "Release To Enter/Exit Wireless Config!" at the 5s threshold.
    void showConfigRelease(bool entering);

    // Show "Hold Button to Exit Wireless Mode to Switch Profiles" for 2s.
    void showWifiLockMessage();

    // Turn display off.
    void off();

    // Update daemon status and redraw if status screen is currently showing.
    void setDaemonStatus(DaemonStatus status);
    DaemonStatus getDaemonStatus() const { return daemonStatus_; }

    // Notify the display module when WiFi mode changes so the status screen
    // shows or hides the "WiFi: Active" line correctly.
    void setWifiActive(bool active);

private:
    Adafruit_SSD1306& oled_;
    bool              ok_           = false;
    DaemonStatus      daemonStatus_ = DaemonStatus::Unknown;
    bool              wifiActive_   = false;

    bool     timerActive_   = false;
    uint32_t timerStart_    = 0;
    uint32_t timerDuration_ = 0;

    enum class TimerAction { TurnOff, ShowStatus };
    TimerAction timerAction_ = TimerAction::TurnOff;

    String lastProfile_;
    bool   lastAlwaysOn_ = false;

    void        drawStatus(const String& profileName, DaemonStatus status);
    void        drawProgressBar(uint8_t filled, uint8_t total, uint8_t y);
    const char* daemonStatusStr(DaemonStatus s);

    static constexpr uint32_t STATUS_TIMEOUT_MS     = 2000;
    static constexpr uint32_t IR_CONFIRM_TIMEOUT_MS = 2000;
    static constexpr uint32_t WIFI_MSG_TIMEOUT_MS   = 2000;
    static constexpr uint32_t CONFIG_MS             = 5000;
    static constexpr uint32_t RESET_START_MS        = 8000;
    static constexpr uint32_t RESET_END_MS          = 23000;
};
