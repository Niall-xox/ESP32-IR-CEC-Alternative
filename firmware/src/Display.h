#pragma once

// Display — OLED state management for the 128x32 SSD1306.
//
// The display has several distinct states that determine what is shown:
//
//   OFF        — blank, display powered down visually (clearDisplay + display())
//   STATUS     — profile name + daemon connection status
//   IR_CONFIRM — "TV On" or "TV Off" shown for 2 seconds after an IR command
//   HOLD_BAR   — progress bar shown while button is held (config mode countdown)
//   RESET_BAR  — progress bar shown while button is held past config threshold
//   WIFI_MSG   — "Hold Button to Exit Wireless Mode..." shown on press 2 in WiFi mode
//
// Timed states (STATUS in non-always-on mode, IR_CONFIRM, WIFI_MSG) expire
// automatically — call update() every loop() to handle expiry.

#include <Arduino.h>
#include <Adafruit_SSD1306.h>

// Daemon connection status — set externally after PING/PONG exchange
enum class DaemonStatus {
    Unknown,    // No PING sent yet this session
    Waiting,    // PING sent, awaiting PONG
    Connected,  // PONG received
    NotFound    // PING timed out
};

class Display {
public:
    // oled — reference to the already-initialised SSD1306 instance in main.cpp
    Display(Adafruit_SSD1306& oled);

    // Call once in setup() after display.begin() succeeds
    void begin();

    // Call on every loop() iteration — handles timed state expiry
    void update();

    // --- State setters ---

    // Show the status screen (profile name + daemon status).
    // In non-always-on mode, starts a 2-second timer then turns off.
    // In always-on mode, stays on indefinitely.
    void showStatus(const String& profileName, DaemonStatus status, bool alwaysOn);

    // Show "TV On" or "TV Off" for 2 seconds then turn off (or return to
    // status screen if always-on is enabled).
    void showIRConfirm(bool on, bool alwaysOn, const String& profileName);

    // Show the hold progress bar (config mode countdown).
    // Called repeatedly by Button::onHold with the current held ms.
    // alwaysOn and profileName are needed to know what to restore if cancelled.
    void showHoldBar(uint32_t heldMs, bool enteringWifi, bool alwaysOn, const String& profileName);

    // Show the factory reset countdown bar.
    // Called repeatedly once heldMs >= 8000.
    void showResetBar(uint32_t heldMs);

    // Show "Release To Enter/Exit Wireless Config!" at the 5s threshold.
    void showConfigRelease(bool entering);

    // Show "Hold Button to Exit Wireless Mode to Switch Profiles" for 2 seconds.
    void showWifiLockMessage();

    // Turn the display off (blank)
    void off();

    // Update the daemon status line without changing display state.
    // Used to refresh the status screen after PONG received.
    void setDaemonStatus(DaemonStatus status);
    DaemonStatus getDaemonStatus() const { return daemonStatus_; }

private:
    Adafruit_SSD1306& oled_;
    bool              ok_           = false;
    DaemonStatus      daemonStatus_ = DaemonStatus::Unknown;

    // Timed state — used to auto-expire STATUS and IR_CONFIRM screens
    bool     timerActive_    = false;
    uint32_t timerStart_     = 0;
    uint32_t timerDuration_  = 0;

    // What to do when timer expires
    enum class TimerAction { TurnOff, ShowStatus };
    TimerAction timerAction_ = TimerAction::TurnOff;

    // Retained for timer expiry restoration
    String   lastProfile_;
    bool     lastAlwaysOn_ = false;

    // Internal draw helpers
    void drawStatus(const String& profileName, DaemonStatus status);
    void drawProgressBar(uint8_t filled, uint8_t total, uint8_t y);
    const char* daemonStatusStr(DaemonStatus s);

    static constexpr uint32_t STATUS_TIMEOUT_MS     = 2000;
    static constexpr uint32_t IR_CONFIRM_TIMEOUT_MS = 2000;
    static constexpr uint32_t WIFI_MSG_TIMEOUT_MS   = 2000;

    // Hold bar timing — must match Button thresholds
    static constexpr uint32_t CONFIG_MS      = 5000;
    static constexpr uint32_t RESET_START_MS = 8000;
    static constexpr uint32_t RESET_END_MS   = 23000;
};
