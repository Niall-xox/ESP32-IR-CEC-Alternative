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
//   ???  → ERR (unknown command)
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

// --- Pin config ---
#define IR_TX_PIN   4   // GPIO connected to the IR LED
#define OLED_SDA    2   // I2C data line
#define OLED_SCL    3   // I2C clock line

// --- OLED config ---
#define OLED_WIDTH  128
#define OLED_HEIGHT 32
#define OLED_ADDR   0x3C

// --- USB HID config ---
// Development placeholder VID/PID — must match HIDTransport in the daemon.
// Replace with a registered VID/PID before commercial release.
#define DEVICE_VID  0x1234
#define DEVICE_PID  0x5678

// HID report size — 64 bytes is the maximum for USB full-speed HID
#define REPORT_SIZE 64

// Vendor-defined HID report descriptor.
// One 64-byte input report (ESP32 → PC) and one 64-byte output report (PC → ESP32).
// Usage page 0xFF00 is vendor-defined — the OS will not treat this as a keyboard or mouse.
static const uint8_t REPORT_DESCRIPTOR[] = {
    0x06, 0x00, 0xFF,        // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x01,              // Usage (Vendor Usage 1)
    0xA1, 0x01,              // Collection (Application)
    0x09, 0x01,              //   Usage (Input Data)
    0x15, 0x00,              //   Logical Minimum (0)
    0x26, 0xFF, 0x00,        //   Logical Maximum (255)
    0x75, 0x08,              //   Report Size (8 bits)
    0x95, REPORT_SIZE,       //   Report Count (64 bytes)
    0x81, 0x02,              //   Input (Data, Variable, Absolute) — ESP32 → PC
    0x09, 0x02,              //   Usage (Output Data)
    0x15, 0x00,              //   Logical Minimum (0)
    0x26, 0xFF, 0x00,        //   Logical Maximum (255)
    0x75, 0x08,              //   Report Size (8 bits)
    0x95, REPORT_SIZE,       //   Report Count (64 bytes)
    0x91, 0x02,              //   Output (Data, Variable, Absolute) — PC → ESP32
    0xC0                     // End Collection
};

// -1 = no hardware reset pin; display is reset over I2C
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
IRsend irSend(IR_TX_PIN);
USBHID HID;

// Tracks whether the OLED initialised successfully.
// All display calls check this flag so a failed OLED doesn't halt the program.
bool oledOk = false;

// Clears the display and writes up to two lines of status text.
// line2 is optional — pass nullptr to leave the lower half blank.
void oledShow(const char* line1, const char* line2 = nullptr) {
    if (!oledOk) return;
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(line1);
    if (line2) {
        display.setCursor(0, 16);
        display.println(line2);
    }
    display.display();
}

// Custom HID device class.
// Registered with the USB HID stack before USB.begin() is called.
class VendorHID : public USBHIDDevice {
public:
    VendorHID() {}

    void begin() {
        HID.addDevice(this, sizeof(REPORT_DESCRIPTOR));
    }

    // Called by the USB stack when the host requests the HID report descriptor
    uint16_t _onGetDescriptor(uint8_t* dst) override {
        memcpy(dst, REPORT_DESCRIPTOR, sizeof(REPORT_DESCRIPTOR));
        return sizeof(REPORT_DESCRIPTOR);
    }

    // Called by the USB stack when an output report is received from the PC
    void _onOutput(uint8_t report_id, const uint8_t* buffer, uint16_t len) override {
        memcpy(rxBuf_, buffer, min((int)len, REPORT_SIZE));
        received_ = true;
    }

    // Send a 64-byte input report to the PC
    bool send(const uint8_t* data) {
        return HID.SendReport(0, data, REPORT_SIZE);
    }

    bool    received_         = false;
    uint8_t rxBuf_[REPORT_SIZE] = {0};
};

VendorHID device;

// ---------------------------------------------------------------------------
// IR dispatch
// Sends the correct IR signal for the given profile using the appropriate
// protocol. sendNEC/sendSAMSUNG/sendSony are all synchronous — they block
// until the full signal has been transmitted. ACK is sent only after this
// returns, guaranteeing the daemon releases its inhibitor lock only after
// the IR signal has actually fired.
// ---------------------------------------------------------------------------
void sendIR(const Profile& profile, bool on) {
    uint32_t code = on ? profile.onCode : profile.offCode;
    switch (profile.protocol) {
        case IrProtocol::SAMSUNG:
            irSend.sendSAMSUNG(code, kSamsungBits);
            break;
        case IrProtocol::SONY:
            // Sony uses variable bit lengths. 20-bit covers most modern Sony TVs.
            irSend.sendSony(code, kSony20Bits);
            break;
        case IrProtocol::NEC:
        default:
            irSend.sendNEC(code, kNECBits);
            break;
    }
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);

    // --- Profiles (LittleFS) ---
    // Must be initialised before anything that reads the active profile.
    Profiles::begin();

    // --- OLED ---
    Wire.begin(OLED_SDA, OLED_SCL);
    if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        oledOk = true;
        // Phase 3: display starts off — button wakes it
        display.clearDisplay();
        display.display();
    }

    // --- IR ---
    irSend.begin();

    // --- USB HID ---
    device.begin();
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
    if (!device.received_) return;
    device.received_ = false;

    // Extract null-terminated command string from the report payload
    String cmd = String((char*)device.rxBuf_);
    cmd.trim();

    uint8_t response[REPORT_SIZE] = {0};

    if (cmd == "ON") {
        sendIR(Profiles::getActive(), true);
        oledShow("TV On");
        response[0] = 'A'; response[1] = 'C'; response[2] = 'K';

    } else if (cmd == "OFF") {
        sendIR(Profiles::getActive(), false);
        oledShow("TV Off");
        response[0] = 'A'; response[1] = 'C'; response[2] = 'K';

    } else if (cmd == "PING") {
        // Daemon liveness check — respond immediately, no IR action
        response[0] = 'P'; response[1] = 'O'; response[2] = 'N'; response[3] = 'G';

    } else {
        oledShow("HID Err", cmd.c_str());
        response[0] = 'E'; response[1] = 'R'; response[2] = 'R';
    }

    device.send(response);

    // Brief confirmation display for ON/OFF, then turn display off.
    // PING and ERR do not trigger the delay — only IR commands do.
    // The ACK is already sent above so this delay does not affect the daemon.
    if (cmd == "ON" || cmd == "OFF") {
        delay(2000);
        display.clearDisplay();
        display.display();
    }
}
