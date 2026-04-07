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
//   PING → respond PONG (daemon liveness check, triggered by button press)
//   ???  → ERR
//
// The daemon holds a systemd inhibitor lock and releases it only after
// receiving ACK, guaranteeing the IR signal has fired before the system
// sleeps or shuts down.

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
#define IR_TX_PIN    4   // GPIO connected to the IR LED
#define OLED_SDA     2   // I2C data line
#define OLED_SCL     3   // I2C clock line
#define BUTTON_PIN   5   // Tactile button (other leg to GND)

// --- OLED config ---
#define OLED_WIDTH  128
#define OLED_HEIGHT  32
#define OLED_ADDR   0x3C

// --- USB HID config ---
#define DEVICE_VID  0x1234
#define DEVICE_PID  0x5678
#define REPORT_SIZE    64

// Vendor-defined HID report descriptor.
// One 64-byte input report (ESP32 → PC) and one 64-byte output report (PC → ESP32).
// Usage page 0xFF00 — the OS will not treat this as a keyboard or mouse.
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

// True when WiFi config mode is active (AP running, web server serving).
// Phase 3 step 6 will set this — for now the button thresholds are wired
// but WiFi itself is not started yet.
bool wifiActive = false;

// Press counter — tracks whether this is the first or second press in a
// display-on window, used to distinguish PING press from cycle press.
// Resets when the display turns off or a hold begins.
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
// IR dispatch
// Sends the correct IR signal for the given profile and direction.
// All send functions are synchronous — ACK is only sent after this returns.
// ---------------------------------------------------------------------------
void sendIR(const Profile& profile, bool on) {
    uint32_t code = on ? profile.onCode : profile.offCode;
    switch (profile.protocol) {
        case IrProtocol::SAMSUNG:
            irSend.sendSAMSUNG(code, kSamsungBits);
            break;
        case IrProtocol::SONY:
            // 20-bit covers most modern Sony TVs
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
// Sends a PING to the daemon and waits briefly for PONG.
// Updates the display daemon status line with the result.
// Called on every first button press.
// ---------------------------------------------------------------------------
void pingDaemon() {
    display.setDaemonStatus(DaemonStatus::Waiting);

    uint8_t txBuf[REPORT_SIZE + 1] = {0};
    txBuf[1] = 'P'; txBuf[2] = 'I'; txBuf[3] = 'N'; txBuf[4] = 'G';
    hidDevice.send(txBuf + 1);  // send() takes the payload without the report ID byte

    // Wait up to 300ms for a PONG — short enough that the display feels instant
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

    if (wifiActive) {
        if (pressCount == 1) {
            // First press in WiFi mode: PING and update status
            pingDaemon();
        } else {
            // Second press in WiFi mode: show lock message, reset counter
            display.showWifiLockMessage();
            pressCount = 0;
        }
        return;
    }

    // Normal operation
    bool alwaysOn = Profiles::getSettings().displayAlwaysOn;

    if (pressCount == 1) {
        // First press: PING daemon and show status screen
        pingDaemon();
        display.showStatus(
            Profiles::getActive().name,
            display.getDaemonStatus(),
            alwaysOn
        );
        if (!alwaysOn) pressCount = 1;  // Will be reset when display turns off
    } else {
        // Second press: cycle to next visible profile
        int next = Profiles::nextVisibleIndex();
        Profiles::getMutableSettings().activeProfile = next;
        Profiles::saveSettings();

        display.showStatus(
            Profiles::getActive().name,
            display.getDaemonStatus(),
            alwaysOn
        );
        pressCount = 0;
    }
}

void onButtonHold(uint32_t heldMs) {
    pressCount = 0;  // Cancel any pending press sequence when hold begins
    bool alwaysOn = Profiles::getSettings().displayAlwaysOn;

    if (heldMs >= 8000) {
        // Factory reset countdown bar (8s–23s)
        display.showResetBar(heldMs);
    } else if (heldMs >= 5000) {
        // Config threshold reached — show release prompt
        display.showConfigRelease(!wifiActive);
    } else {
        // Normal hold bar (300ms–5s)
        display.showHoldBar(heldMs, !wifiActive, alwaysOn, Profiles::getActive().name);
    }
}

void onConfigThreshold() {
    // Button released at the 5s config threshold — toggle WiFi mode
    // WiFi start/stop will be implemented in step 6.
    wifiActive = !wifiActive;
    Serial.printf("[app] Wireless config mode: %s\n", wifiActive ? "ON" : "OFF");

    bool alwaysOn = Profiles::getSettings().displayAlwaysOn;
    display.showStatus(Profiles::getActive().name, display.getDaemonStatus(),
                       wifiActive ? true : alwaysOn);
    pressCount = 0;
}

void onFactoryReset() {
    Serial.println("[app] Factory reset triggered");
    Profiles::factoryReset();
    wifiActive = false;
    pressCount = 0;
    display.showStatus(Profiles::getActive().name, DaemonStatus::Unknown, false);
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);

    // --- Profiles (LittleFS) ---
    Profiles::begin();

    // --- OLED ---
    Wire.begin(OLED_SDA, OLED_SCL);
    if (oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        display.begin();  // Clears display, starts off
    }

    // --- Button ---
    button.begin();
    button.onPress         = onButtonPress;
    button.onHold          = onButtonHold;
    button.onConfigThreshold = onConfigThreshold;
    button.onFactoryReset  = onFactoryReset;

    // --- IR ---
    irSend.begin();

    // --- USB HID ---
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
    // Drive button timing and display timer expiry
    button.update();
    display.update();

    // --- HID command handling ---
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
        // Daemon liveness check from the PC side (distinct from button-triggered PING)
        response[0] = 'P'; response[1] = 'O'; response[2] = 'N'; response[3] = 'G';

    } else {
        display.showStatus(Profiles::getActive().name, display.getDaemonStatus(),
                           Profiles::getSettings().displayAlwaysOn || wifiActive);
        response[0] = 'E'; response[1] = 'R'; response[2] = 'R';
    }

    hidDevice.send(response);
}
