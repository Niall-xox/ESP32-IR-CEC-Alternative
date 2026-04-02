// HID Two-Way Communication Test — ESP32-S3 Firmware
//
// Presents the ESP32-S3 as a vendor-defined USB HID device.
// Receives 64-byte output reports from the PC and echoes them back
// as input reports with "ACK:" prepended, verifying two-way HID communication.
//
// PC → ESP32: any 64-byte payload
// ESP32 → PC: "ACK:" + first 60 bytes of received payload
//
// OLED display states:
//   "Waiting..."       — ready, no report received yet
//   "RX: <payload>"   — showing first 12 chars of received payload
//   "ACK sent"        — response successfully sent to PC

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "USB.h"
#include "USBHID.h"

// USB identifiers — these will become the permanent VID/PID in Phase 2.
// The PC test program uses these to find the device.
#define TEST_VID 0x1234
#define TEST_PID 0x5678

// HID report size — 64 bytes is the maximum for USB full-speed HID
#define REPORT_SIZE 64

// OLED pin config — matches main firmware
#define OLED_SDA    2
#define OLED_SCL    3
#define OLED_WIDTH  128
#define OLED_HEIGHT 32
#define OLED_ADDR   0x3C

// Vendor-defined HID report descriptor.
// Defines one 64-byte input report (ESP32 → PC) and one 64-byte output report (PC → ESP32).
// Usage page 0xFF00 is the vendor-defined range, so no OS will try to interpret this
// as a keyboard, mouse, or other standard device.
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
bool oledOk = false;

// Clears the display and writes up to two lines of status text
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

USBHID HID;

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

    bool received_ = false;
    uint8_t rxBuf_[REPORT_SIZE] = {0};
};

VendorHID device;

void setup() {
    // OLED init
    Wire.begin(OLED_SDA, OLED_SCL);
    if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        oledOk = true;
        oledShow("Waiting...");
    }

    device.begin();

    USB.VID(TEST_VID);
    USB.PID(TEST_PID);
    USB.productName("ESP32 HID Test");
    USB.manufacturerName("ESP32-IR-CEC");
    USB.begin();

    HID.begin();
}

void loop() {
    if (!device.received_) return;
    device.received_ = false;

    // Show first 12 chars of the received payload on the OLED
    char preview[13] = {0};
    memcpy(preview, device.rxBuf_, 12);
    char rxLine[32];
    snprintf(rxLine, sizeof(rxLine), "RX: %s", preview);
    oledShow(rxLine);

    // Build ACK response: "ACK:" followed by the first 60 bytes of the received payload
    uint8_t response[REPORT_SIZE] = {0};
    response[0] = 'A';
    response[1] = 'C';
    response[2] = 'K';
    response[3] = ':';
    memcpy(response + 4, device.rxBuf_, REPORT_SIZE - 4);

    if (device.send(response)) {
        oledShow(rxLine, "ACK sent");
    } else {
        oledShow(rxLine, "Send failed");
    }
}
