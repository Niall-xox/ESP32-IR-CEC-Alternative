// Component test — verifies that the IR LED, OLED display, and firmware
// upload toolchain are all working before main firmware development begins.
// Continuously fires LG IR ON/OFF codes and updates the OLED each cycle.
// This file is deprecated once the main firmware is confirmed working.

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
#define OLED_WIDTH  128         // Display width in pixels
#define OLED_HEIGHT 32          // Display height in pixels
#define OLED_ADDR   0x3C        // I2C address (0x3C or 0x3D depending on module)

// LG NEC discrete IR codes (confirmed working on LG C2).
// Discrete codes are used instead of the toggle code so the TV always reaches
// the correct state regardless of what state it was already in.
#define LG_IR_ON     0x20DF23DC
#define LG_IR_OFF    0x20DFA35C

// -1 = no hardware reset pin connected; the display is reset over I2C
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
IRsend irSend(IR_TX_PIN);

// Tracks whether the OLED initialised successfully.
// All display calls check this flag so a failed OLED doesn't crash the program.
bool oledOk = false;

// Clears the display and writes up to two lines of status text.
// line2 is optional — pass nullptr to leave the lower half blank.
void oledStatus(const char* line1, const char* line2 = nullptr) {
    if (!oledOk) return;
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(line1);
    if (line2) {
        display.setCursor(0, 16);  // Second line sits halfway down the 32px display
        display.println(line2);
    }
    display.display();  // Push buffer to the physical display
}

void setup() {
    Serial.begin(115200);
    delay(500);  // Brief pause to let the USB serial connection settle before printing
    Serial.println("=== Component Test ===");

    // Initialise I2C on the specified pins, then bring up the OLED.
    // begin() returns false if the display doesn't ACK on the I2C bus.
    Wire.begin(OLED_SDA, OLED_SCL);
    if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        oledOk = true;
        Serial.println("[OLED] OK");
        oledStatus("Component Test", "Starting...");
    } else {
        Serial.println("[OLED] INIT FAILED");
    }

    // IRsend::begin() configures the PWM/RMT peripheral for the TX pin
    irSend.begin();
    Serial.println("[IR]   OK");

    delay(1000);
}

void loop() {
    // Fire the discrete ON code and show it on the display.
    // 32 = number of bits in the NEC payload.
    Serial.println("[IR] Sending LG ON");
    oledStatus("IR Test", "Sending ON...");
    irSend.sendNEC(LG_IR_ON, 32);
    delay(2000);

    // Fire the discrete OFF code — TV should turn off if it was on.
    Serial.println("[IR] Sending LG OFF");
    oledStatus("IR Test", "Sending OFF...");
    irSend.sendNEC(LG_IR_OFF, 32);
    delay(2000);
}
