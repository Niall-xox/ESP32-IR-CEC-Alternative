// ESP32 IR Remote — Firmware (Phase 1)
//
// Listens for commands from the PC daemon over USB CDC Serial and fires the
// corresponding IR signal at the TV. Updates the OLED display after each action.
//
// Communication protocol (Phase 1):
//   PC → ESP32:  "ON\n"  — send discrete IR power-on code
//   PC → ESP32:  "OFF\n" — send discrete IR power-off code
//   No response is sent back to the PC in Phase 1. The daemon uses a fixed
//   delay to allow time for the IR signal to fire before releasing its
//   systemd inhibitor lock.

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>

// --- Pin config ---
#define IR_TX_PIN   4   // GPIO connected to the IR LED
#define OLED_SDA    2   // I2C data line
#define OLED_SCL    3   // I2C clock line

// --- OLED config ---
#define OLED_WIDTH  128
#define OLED_HEIGHT 32
#define OLED_ADDR   0x3C

// --- Serial config ---
#define SERIAL_BAUD     115200
#define SERIAL_TIMEOUT  200    // ms to wait for a complete line before giving up

// LG NEC discrete IR codes (confirmed working on LG C2).
// Discrete codes guarantee the correct TV state regardless of prior state —
// safer than the toggle code which would misbehave if state has drifted.
#define LG_IR_ON     0x20DF23DC
#define LG_IR_OFF    0x20DFA35C

// -1 = no hardware reset pin; display is reset over I2C
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
IRsend irSend(IR_TX_PIN);

// Tracks whether the OLED initialised successfully.
// All display calls check this flag so a failed OLED doesn't halt the program —
// IR control must still work even if the display is absent or broken.
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
        display.setCursor(0, 16);  // Second line at vertical midpoint of 32px display
        display.println(line2);
    }
    display.display();  // Push buffer to the physical display
}

void setup() {
    Serial.begin(SERIAL_BAUD);
    Serial.setTimeout(SERIAL_TIMEOUT);
    delay(500);  // Let the USB serial connection settle before printing

    // --- OLED ---
    // Initialise I2C on the specified pins then bring up the display.
    // If begin() fails the display flag stays false and the error is shown
    // only over Serial (since the display itself can't be trusted).
    Wire.begin(OLED_SDA, OLED_SCL);
    if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        oledOk = true;
        oledShow("Waiting...");  // Idle state — ready for commands
        Serial.println("[OLED] OK");
    } else {
        Serial.println("[OLED] INIT FAILED");
    }

    // --- IR ---
    // Configures the RMT peripheral on the TX pin for NEC modulation.
    irSend.begin();
    Serial.println("[IR]   OK");

    Serial.println("[SYS]  Ready");
}

void loop() {
    // Block until a newline-terminated command arrives, or SERIAL_TIMEOUT elapses.
    // readStringUntil returns an empty string on timeout, so unintended triggers
    // from partial reads are avoided.
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();  // Strip any trailing \r or whitespace

    if (cmd.length() == 0) {
        return;  // Timeout with no data — nothing to do
    }

    if (cmd == "ON") {
        Serial.println("[CMD] ON received");
        irSend.sendNEC(LG_IR_ON, kNECBits);
        oledShow("TV On");
        Serial.println("[IR]  ON sent");

    } else if (cmd == "OFF") {
        Serial.println("[CMD] OFF received");
        irSend.sendNEC(LG_IR_OFF, kNECBits);
        oledShow("TV Off");
        Serial.println("[IR]  OFF sent");

    } else {
        // Unknown command — log it but don't act. Keeps the device fault-tolerant
        // against malformed input without crashing or requiring a reset.
        Serial.print("[ERR] Unknown command: ");
        Serial.println(cmd);
        oledShow("Serial Err", cmd.c_str());
    }
}
