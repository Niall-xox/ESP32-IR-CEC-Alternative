#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "USB.h"
#include "USBHID.h"

#define TEST_VID 0x1234
#define TEST_PID 0x5678

#define REPORT_SIZE 64

#define OLED_SDA    2
#define OLED_SCL    3
#define OLED_WIDTH  128
#define OLED_HEIGHT 32
#define OLED_ADDR   0x3C

static const uint8_t REPORT_DESCRIPTOR[] = {
    0x06, 0x00, 0xFF,
    0x09, 0x01,
    0xA1, 0x01,
    0x09, 0x01,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x95, REPORT_SIZE,
    0x81, 0x02,
    0x09, 0x02,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x95, REPORT_SIZE,
    0x91, 0x02,
    0xC0
};

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
bool oledOk = false;

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

class VendorHID : public USBHIDDevice {
public:
    VendorHID() {}

    void begin() {
        HID.addDevice(this, sizeof(REPORT_DESCRIPTOR));
    }

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

    bool received_ = false;
    uint8_t rxBuf_[REPORT_SIZE] = {0};
};

VendorHID device;

void setup() {

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

    char preview[13] = {0};
    memcpy(preview, device.rxBuf_, 12);
    char rxLine[32];
    snprintf(rxLine, sizeof(rxLine), "RX: %s", preview);
    oledShow(rxLine);

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
