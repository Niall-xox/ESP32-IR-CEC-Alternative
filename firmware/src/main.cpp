// ESP32 IR Remote — Firmware (Phase 3)
//
// Presents the ESP32-S3 as a USB HID device. Receives commands from the PC
// daemon as HID output reports, fires the IR signal for the active manufacturer
// profile, updates the OLED display, and sends a response as an input report.
//
// Communication protocol:
//   PC → ESP32:  64-byte output report, command string in first bytes
//   ESP32 → PC:  64-byte input report, response string in first bytes
//
//   ON   → fire IR ON  for active profile → ACK
//   OFF  → fire IR OFF for active profile → ACK
//   PING → respond PONG (daemon liveness check — sent by button press or daemon)
//   ???  → ERR

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include "USB.h"
#include "USBHID.h"
#include "Profiles.h"
#include "Button.h"
#include "Display.h"

// --- Pin config ---
#define IR_TX_PIN    4
#define OLED_SDA     2
#define OLED_SCL     3
#define BUTTON_PIN   5

// --- OLED config ---
#define OLED_WIDTH  128
#define OLED_HEIGHT  32
#define OLED_ADDR   0x3C

// --- USB HID config ---
#define DEVICE_VID  0x1234
#define DEVICE_PID  0x5678
#define REPORT_SIZE    64

static const uint8_t REPORT_DESCRIPTOR[] = {
    0x06, 0x00, 0xFF,
    0x09, 0x01,
    0xA1, 0x01,
    0x09, 0x01,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x95, REPORT_SIZE,
    0x81, 0x02,              // Input  (ESP32 → PC)
    0x09, 0x02,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x95, REPORT_SIZE,
    0x91, 0x02,              // Output (PC → ESP32)
    0xC0
};

// ---------------------------------------------------------------------------
// Hardware instances
// ---------------------------------------------------------------------------
Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
IRsend           irSend(IR_TX_PIN);
USBHID           HID;
Button           button(BUTTON_PIN);
Display          display(oled);

// ---------------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------------
bool    wifiActive = false;

// Tracks whether this is the first or second press in a display-on window.
// Reset to 0 when display turns off (via display.onExpire) or a hold begins.
uint8_t pressCount = 0;

// ---------------------------------------------------------------------------
// USB HID device class
// ---------------------------------------------------------------------------
class VendorHID : public USBHIDDevice {
public:
    VendorHID() {}

    void begin() { HID.addDevice(this, sizeof(REPORT_DESCRIPTOR)); }

    uint16_t _onGetDescriptor(uint8_t* dst) override {
        memcpy(dst, REPORT_DESCRIPTOR, sizeof(REPORT_DESCRIPTOR));
        return sizeof(REPORT_DESCRIPTOR);
    }

    void _onOutput(uint8_t report_id, const uint8_t* buffer, uint16_t len) override {
        memcpy(rxBuf_, buffer, min((int)len, REPORT_SIZE));
        received_ = true;
    }

    bool send(const uint8_t* data) {
        return HID.SendReport(0, data, REPORT_SIZE);
    }

    bool    received_           = false;
    uint8_t rxBuf_[REPORT_SIZE] = {0};
};

VendorHID hidDevice;

// ---------------------------------------------------------------------------
// IR dispatch — synchronous, blocks until the full signal is transmitted
// ---------------------------------------------------------------------------
void sendIR(const Profile& profile, bool on) {
    uint32_t code = on ? profile.onCode : profile.offCode;
    switch (profile.protocol) {
        case IrProtocol::SAMSUNG:
            irSend.sendSAMSUNG(code, kSamsungBits);
            break;
        case IrProtocol::SONY:
            irSend.sendSony(code, kSony20Bits);
            break;
        case IrProtocol::NEC:
        default:
            irSend.sendNEC(code, kNECBits);
            break;
    }
}

// ---------------------------------------------------------------------------
// PING helper
// Shows "Daemon: Waiting..." immediately, sends PING as an input report,
// waits up to 300ms for a PONG output report from the daemon background reader,
// then updates the display with Connected or Not Found.
// ---------------------------------------------------------------------------
void pingDaemon(bool alwaysOn) {
    // Show status with Waiting first so the display updates before the PING
    display.showStatus(Profiles::getActive().name, DaemonStatus::Waiting, alwaysOn);

    // Send PING as an HID input report (ESP32 → PC).
    // The daemon background reader thread will see this and send PONG back.
    uint8_t txBuf[REPORT_SIZE] = {0};
    txBuf[0] = 'P'; txBuf[1] = 'I'; txBuf[2] = 'N'; txBuf[3] = 'G';
    hidDevice.send(txBuf);

    // Wait up to 300ms for PONG to arrive as an output report via _onOutput
    uint32_t start = millis();
    while ((millis() - start) < 300) {
        if (hidDevice.received_) {
            hidDevice.received_ = false;
            String resp = String((char*)hidDevice.rxBuf_);
            if (resp.startsWith("PONG")) {
                display.setDaemonStatus(DaemonStatus::Connected);
                return;
            }
        }
        delay(10);
    }
    display.setDaemonStatus(DaemonStatus::NotFound);
}

// ---------------------------------------------------------------------------
// Button callbacks
// ---------------------------------------------------------------------------
void onButtonPress() {
    pressCount++;
    bool alwaysOn = Profiles::getSettings().displayAlwaysOn || wifiActive;

    if (wifiActive) {
        if (pressCount == 1) {
            // First press in WiFi mode: PING and refresh status screen
            pingDaemon(true);  // alwaysOn=true in WiFi mode
        } else {
            // Second press in WiFi mode: show lock message, reset counter
            display.showWifiLockMessage();
            pressCount = 0;
        }
        return;
    }

    // Normal operation
    if (pressCount == 1) {
        // First press: PING and show status screen
        pingDaemon(alwaysOn);
        // In non-always-on mode the display is now on with a 2s timer.
        // pressCount stays at 1 until the timer fires onExpire (which resets to 0)
        // or the user presses again (which increments to 2).
    } else {
        // Second press: cycle to next visible profile and refresh display
        int next = Profiles::nextVisibleIndex();
        Profiles::getMutableSettings().activeProfile = next;
        Profiles::saveSettings();

        display.showStatus(Profiles::getActive().name, display.getDaemonStatus(), alwaysOn);
        pressCount = 0;
    }
}

void onButtonHold(uint32_t heldMs) {
    // A hold cancels any in-progress press sequence
    pressCount = 0;
    bool alwaysOn = Profiles::getSettings().displayAlwaysOn || wifiActive;

    if (heldMs >= 8000) {
        display.showResetBar(heldMs);
    } else if (heldMs >= 5000) {
        display.showConfigRelease(!wifiActive);
    } else {
        display.showHoldBar(heldMs, !wifiActive, alwaysOn, Profiles::getActive().name);
    }
}

void onConfigThreshold() {
    // Button released at 5s — toggle WiFi config mode
    wifiActive = !wifiActive;
    display.setWifiActive(wifiActive);  // Tell Display so it shows/hides WiFi line
    Serial.printf("[app] Wireless config mode: %s\n", wifiActive ? "ON" : "OFF");

    // WiFi start/stop implemented in step 6 — toggle is wired, AP not started yet
    bool alwaysOn = Profiles::getSettings().displayAlwaysOn || wifiActive;
    display.showStatus(Profiles::getActive().name, display.getDaemonStatus(), alwaysOn);
    pressCount = 0;
}

void onFactoryReset() {
    Serial.println("[app] Factory reset triggered");
    Profiles::factoryReset();
    wifiActive = false;
    display.setWifiActive(false);
    pressCount = 0;
    bool alwaysOn = Profiles::getSettings().displayAlwaysOn;
    display.showStatus(Profiles::getActive().name, DaemonStatus::Unknown, alwaysOn);
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);

    Profiles::begin();

    Wire.begin(OLED_SDA, OLED_SCL);
    if (oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        display.begin();
    }

    button.begin();
    button.onPress           = onButtonPress;
    button.onHold            = onButtonHold;
    button.onConfigThreshold = onConfigThreshold;
    button.onFactoryReset    = onFactoryReset;

    // When the STATUS screen timer expires and the display turns off,
    // reset pressCount so the next press is treated as a first press again.
    display.onExpire = []() { pressCount = 0; };

    irSend.begin();

    hidDevice.begin();
    USB.VID(DEVICE_VID);
    USB.PID(DEVICE_PID);
    USB.productName("ESP32 IR Remote");
    USB.manufacturerName("ESP32-IR-CEC");
    USB.begin();
    HID.begin();
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------
void loop() {
    button.update();
    display.update();

    if (!hidDevice.received_) return;
    hidDevice.received_ = false;

    String cmd = String((char*)hidDevice.rxBuf_);
    cmd.trim();

    uint8_t response[REPORT_SIZE] = {0};

    if (cmd == "ON") {
        sendIR(Profiles::getActive(), true);
        display.showIRConfirm(true,
                              Profiles::getSettings().displayAlwaysOn || wifiActive,
                              Profiles::getActive().name);
        response[0] = 'A'; response[1] = 'C'; response[2] = 'K';

    } else if (cmd == "OFF") {
        sendIR(Profiles::getActive(), false);
        display.showIRConfirm(false,
                              Profiles::getSettings().displayAlwaysOn || wifiActive,
                              Profiles::getActive().name);
        response[0] = 'A'; response[1] = 'C'; response[2] = 'K';

    } else if (cmd == "PING") {
        // Daemon-initiated PING (distinct from button-triggered PING)
        response[0] = 'P'; response[1] = 'O'; response[2] = 'N'; response[3] = 'G';

    } else {
        response[0] = 'E'; response[1] = 'R'; response[2] = 'R';
    }

    hidDevice.send(response);
}
