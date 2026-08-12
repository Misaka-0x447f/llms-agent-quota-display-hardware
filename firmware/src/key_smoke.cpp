#include <Arduino.h>
#include <U8g2lib.h>

namespace {
constexpr uint8_t kI2cClock = 9;
constexpr uint8_t kI2cData = 8;
constexpr uint32_t kDebounceMs = 25;

U8G2_SSD1315_128X64_NONAME_F_SW_I2C display(
    U8G2_R0, kI2cClock, kI2cData, U8X8_PIN_NONE);

struct Key {
  const char* label;
  uint8_t pin;
  bool rawPressed;
  bool stablePressed;
  uint32_t changedAtMs;
};

Key keys[] = {
    {"K4 * GPIO3", 3, false, false, 0},
    {"K3 # GPIO2", 2, false, false, 0},
    {"K2 v GPIO1", 1, false, false, 0},
    {"K1 ^ GPIO0", 0, false, false, 0},
};

void drawKeys() {
  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(0, 9, "KEY TEST");
  for (uint8_t i = 0; i < 4; ++i) {
    const String line = String(keys[i].label) +
                        (keys[i].stablePressed ? " [X]" : " [ ]");
    display.drawStr(0, 21 + i * 11, line.c_str());
  }
  display.sendBuffer();
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  for (auto& key : keys) {
    pinMode(key.pin, INPUT_PULLUP);
    key.rawPressed = digitalRead(key.pin) == LOW;
    key.stablePressed = key.rawPressed;
  }

  display.setI2CAddress(0x3c << 1);
  display.setBusClock(100000);
  display.begin();
  drawKeys();
  Serial.println("KEY_TEST_READY K4=GPIO3 K3=GPIO2 K2=GPIO1 K1=GPIO0");
}

void loop() {
  const uint32_t now = millis();
  for (auto& key : keys) {
    const bool pressed = digitalRead(key.pin) == LOW;
    if (pressed != key.rawPressed) {
      key.rawPressed = pressed;
      key.changedAtMs = now;
    }
    if (key.stablePressed != key.rawPressed &&
        now - key.changedAtMs >= kDebounceMs) {
      key.stablePressed = key.rawPressed;
      Serial.printf("KEY %s %s\n", key.label,
                    key.stablePressed ? "DOWN" : "UP");
      drawKeys();
    }
  }
  delay(2);
}
