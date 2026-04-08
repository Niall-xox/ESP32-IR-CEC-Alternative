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
//   ???  → ERR

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
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

// --- WiFi AP config ---
#define WIFI_SSID  "ESP32-IR-Remote"
#define WIFI_PASS  "irremote123"

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
WebServer        server(80);

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
        uint16_t copyLen = min((int)len, REPORT_SIZE - 1);
        memcpy(rxBuf_, buffer, copyLen);
        rxBuf_[copyLen] = '\0';
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
// WiFi AP + Web Server
// ---------------------------------------------------------------------------
void stopWifi();

void startWifi() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(WIFI_SSID, WIFI_PASS);
    Serial.printf("[wifi] AP started — SSID: %s  IP: %s\n",
                  WIFI_SSID, WiFi.softAPIP().toString().c_str());

    // --- API endpoints ---

    // GET /api/profiles — return all profiles as JSON array
    server.on("/api/profiles", HTTP_GET, []() {
        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();
        for (const auto& p : Profiles::getAll()) {
            JsonObject obj = arr.add<JsonObject>();
            char onBuf[11], offBuf[11];
            snprintf(onBuf,  sizeof(onBuf),  "0x%08X", p.onCode);
            snprintf(offBuf, sizeof(offBuf), "0x%08X", p.offCode);
            obj["name"]     = p.name;
            obj["protocol"] = Profiles::protocolToString(p.protocol);
            obj["on"]       = onBuf;
            obj["off"]      = offBuf;
            obj["visible"]  = p.visible;
        }
        String json;
        serializeJson(doc, json);
        server.send(200, "application/json", json);
    });

    // POST /api/profiles — replace all profiles from JSON body
    server.on("/api/profiles", HTTP_POST, []() {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, server.arg("plain"));
        if (err) {
            server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
            return;
        }
        // Replace in-memory profiles and save
        std::vector<Profile> newProfiles;
        for (JsonObject obj : doc.as<JsonArray>()) {
            Profile p;
            p.name     = obj["name"].as<String>();
            p.protocol = Profiles::protocolFromString(obj["protocol"].as<String>());
            p.onCode   = strtoul(obj["on"].as<const char*>(), nullptr, 16);
            p.offCode  = strtoul(obj["off"].as<const char*>(), nullptr, 16);
            p.visible  = obj["visible"] | true;
            newProfiles.push_back(p);
        }
        if (newProfiles.empty()) {
            server.send(400, "application/json", "{\"error\":\"At least one profile required\"}");
            return;
        }
        // Overwrite in-memory state via Profiles API
        auto& all = const_cast<std::vector<Profile>&>(Profiles::getAll());
        all = std::move(newProfiles);
        // Clamp active profile
        if (Profiles::getSettings().activeProfile >= (int)Profiles::getAll().size()) {
            Profiles::getMutableSettings().activeProfile = 0;
        }
        Profiles::saveProfiles();
        Profiles::saveSettings();
        server.send(200, "application/json", "{\"ok\":true}");
    });

    // GET /api/settings — return settings as JSON
    server.on("/api/settings", HTTP_GET, []() {
        const Settings& s = Profiles::getSettings();
        JsonDocument doc;
        doc["active_profile"]    = s.activeProfile;
        doc["display_always_on"] = s.displayAlwaysOn;
        String json;
        serializeJson(doc, json);
        server.send(200, "application/json", json);
    });

    // POST /api/settings — update settings from JSON body
    server.on("/api/settings", HTTP_POST, []() {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, server.arg("plain"));
        if (err) {
            server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
            return;
        }
        Settings& s = Profiles::getMutableSettings();
        if (doc.containsKey("active_profile")) {
            int idx = doc["active_profile"].as<int>();
            if (idx >= 0 && idx < (int)Profiles::getAll().size()) {
                s.activeProfile = idx;
            }
        }
        if (doc.containsKey("display_always_on")) {
            s.displayAlwaysOn = doc["display_always_on"].as<bool>();
        }
        Profiles::saveSettings();
        server.send(200, "application/json", "{\"ok\":true}");
    });

    // POST /api/factory-reset — restore defaults
    server.on("/api/factory-reset", HTTP_POST, []() {
        Profiles::factoryReset();
        server.send(200, "application/json", "{\"ok\":true}");
    });

    // POST /api/exit — save and exit wireless config mode
    server.on("/api/exit", HTTP_POST, []() {
        server.send(200, "application/json", "{\"ok\":true}");
        // Defer stop to next loop() so the response is sent first
        wifiActive = false;
    });

    // Serve static files from LittleFS (web UI)
    server.onNotFound([]() {
        String path = server.uri();
        if (path == "/") path = "/index.html";

        if (LittleFS.exists(path)) {
            String contentType = "text/plain";
            if      (path.endsWith(".html")) contentType = "text/html";
            else if (path.endsWith(".css"))  contentType = "text/css";
            else if (path.endsWith(".js"))   contentType = "application/javascript";
            else if (path.endsWith(".json")) contentType = "application/json";

            File f = LittleFS.open(path, "r");
            server.streamFile(f, contentType);
            f.close();
        } else {
            server.send(404, "text/plain", "Not Found");
        }
    });

    server.begin();
    Serial.println("[wifi] Web server started on port 80");
}

void stopWifi() {
    server.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("[wifi] AP stopped");
}

// ---------------------------------------------------------------------------
// Button callbacks
// ---------------------------------------------------------------------------
void onButtonPress() {
    pressCount++;
    bool alwaysOn = Profiles::getSettings().displayAlwaysOn || wifiActive;

    if (wifiActive) {
        if (pressCount == 1) {
            // First press in WiFi mode: show status screen
            display.showStatus(Profiles::getActive().name, true);
        } else {
            // Second press and beyond in WiFi mode: show lock message.
            display.showWifiLockMessage();
            pressCount = 1;
        }
        return;
    }

    // Normal operation
    if (pressCount == 1) {
        // First press: show status screen
        display.showStatus(Profiles::getActive().name, alwaysOn);
    } else {
        // Second press and beyond: cycle to next visible profile and refresh display.
        int next = Profiles::nextVisibleIndex();
        Profiles::getMutableSettings().activeProfile = next;
        Profiles::saveSettings();

        display.showStatus(Profiles::getActive().name, alwaysOn);
        pressCount = 1;
    }
}

void onButtonHold(uint32_t heldMs) {
    // A hold cancels any in-progress press sequence
    pressCount = 0;
    bool alwaysOn = Profiles::getSettings().displayAlwaysOn || wifiActive;

    if (heldMs >= 23000) {
        // Factory reset has already triggered — stop updating the display so
        // the status screen drawn by onFactoryReset is not overwritten
        return;
    } else if (heldMs >= 8000) {
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
    display.setWifiActive(wifiActive);

    if (wifiActive) {
        startWifi();
    } else {
        stopWifi();
    }

    bool alwaysOn = Profiles::getSettings().displayAlwaysOn || wifiActive;
    display.showStatus(Profiles::getActive().name, alwaysOn);
    pressCount = 1;
}

void onFactoryReset() {
    Serial.println("[app] Factory reset triggered");
    Profiles::factoryReset();
    if (wifiActive) stopWifi();
    wifiActive = false;
    display.setWifiActive(false);
    bool alwaysOn = Profiles::getSettings().displayAlwaysOn;
    display.showStatus(Profiles::getActive().name, alwaysOn);
    pressCount = 1;
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
    button.onHoldCancelled   = []() {
        // Bar was filling but button released before threshold — show status screen.
        bool alwaysOn = Profiles::getSettings().displayAlwaysOn || wifiActive;
        display.showStatus(Profiles::getActive().name, alwaysOn);
        pressCount = 1;
    };
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

    if (wifiActive) {
        server.handleClient();
    } else if (WiFi.getMode() != WIFI_OFF) {
        // Deferred stop from /api/exit — response has been sent, now shut down
        stopWifi();
        display.setWifiActive(false);
        bool alwaysOn = Profiles::getSettings().displayAlwaysOn;
        display.showStatus(Profiles::getActive().name, alwaysOn);
        pressCount = 1;
    }

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

    } else {
        response[0] = 'E'; response[1] = 'R'; response[2] = 'R';
    }

    hidDevice.send(response);
}
