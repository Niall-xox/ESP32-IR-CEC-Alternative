#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>

#define IR_TX_PIN   4
#define OLED_SDA    2
#define OLED_SCL    3

#define OLED_WIDTH  128
#define OLED_HEIGHT 32
#define OLED_ADDR   0x3C

#define LG_IR_ON     0x20DF23DC
#define LG_IR_OFF    0x20DFA35C

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
IRsend irSend(IR_TX_PIN);

bool oledOk = false;

void oledStatus(const char* line1, const char* line2 = nullptr) {
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

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("=== Component Test ===");

    Wire.begin(OLED_SDA, OLED_SCL);
    if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        oledOk = true;
        Serial.println("[OLED] OK");
        oledStatus("Component Test", "Starting...");
    } else {
        Serial.println("[OLED] INIT FAILED");
    }

    irSend.begin();
    Serial.println("[IR]   OK");

    delay(1000);
}

void loop() {

    Serial.println("[IR] Sending LG ON");
    oledStatus("IR Test", "Sending ON...");
    irSend.sendNEC(LG_IR_ON, 32);
    delay(2000);

    Serial.println("[IR] Sending LG OFF");
    oledStatus("IR Test", "Sending OFF...");
    irSend.sendNEC(LG_IR_OFF, 32);
    delay(2000);
}
