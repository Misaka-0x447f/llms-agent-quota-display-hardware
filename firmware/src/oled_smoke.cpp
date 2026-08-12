#include <Arduino.h>
#include <U8g2lib.h>

namespace {
constexpr uint8_t kI2cClock = 9;
constexpr uint8_t kI2cData = 8;

// Software I2C keeps this test independent from Wire and performs no scan.
U8G2_SSD1315_128X64_NONAME_F_SW_I2C display(
    U8G2_R0, kI2cClock, kI2cData, U8X8_PIN_NONE);

bool ackNormal3c = false;
bool ackNormal3d = false;
bool ackSwapped3c = false;
bool ackSwapped3d = false;

void releaseLine(uint8_t pin) {
  pinMode(pin, INPUT_PULLUP);
}

void driveLineLow(uint8_t pin) {
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
}

void busDelay() {
  delayMicroseconds(5);
}

bool probeAddress(uint8_t scl, uint8_t sda, uint8_t address) {
  releaseLine(scl);
  releaseLine(sda);
  busDelay();
  driveLineLow(sda);
  busDelay();
  driveLineLow(scl);

  uint8_t value = address << 1;
  for (uint8_t mask = 0x80; mask != 0; mask >>= 1) {
    if (value & mask) releaseLine(sda);
    else driveLineLow(sda);
    busDelay();
    releaseLine(scl);
    busDelay();
    driveLineLow(scl);
  }

  releaseLine(sda);
  busDelay();
  releaseLine(scl);
  busDelay();
  const bool ack = digitalRead(sda) == LOW;
  driveLineLow(scl);
  driveLineLow(sda);
  busDelay();
  releaseLine(scl);
  busDelay();
  releaseLine(sda);
  busDelay();
  return ack;
}

void pulseDataLed(uint16_t onMs, uint16_t offMs) {
  // The onboard GPIO8 LED is active low on this board. SCL stays high, so
  // changing SDA here cannot form a valid I2C transaction.
  pinMode(kI2cClock, INPUT_PULLUP);
  pinMode(kI2cData, OUTPUT);
  digitalWrite(kI2cData, LOW);
  delay(onMs);
  pinMode(kI2cData, INPUT_PULLUP);
  delay(offMs);
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1000);

  for (uint8_t i = 0; i < 3; ++i) pulseDataLed(300, 300);

  ackNormal3c = probeAddress(9, 8, 0x3c);
  ackNormal3d = probeAddress(9, 8, 0x3d);
  ackSwapped3c = probeAddress(8, 9, 0x3c);
  ackSwapped3d = probeAddress(8, 9, 0x3d);

  display.setI2CAddress(0x3c << 1);
  display.setBusClock(100000);
  display.begin();
  display.clearBuffer();
  display.drawFrame(0, 0, 128, 64);
  display.setFont(u8g2_font_6x12_tf);
  display.drawStr(16, 22, "OLED TEST");
  display.drawStr(10, 40, "SSD1315 @ 0x3C");
  display.drawStr(19, 56, "SCL9  SDA8");
  display.sendBuffer();

  Serial.println("OLED_SMOKE_SENT SSD1306_COMPAT ADDR=0x3C SCL=7 SDA=8");
}

void loop() {
  pulseDataLed(200, 1800);
  Serial.printf(
      "OLED_SMOKE ACK 9/8:3C=%u,3D=%u 8/9:3C=%u,3D=%u IDLE9=%u IDLE8=%u\n",
      ackNormal3c, ackNormal3d, ackSwapped3c, ackSwapped3d,
      digitalRead(9), digitalRead(8));
}
