# 引脚与外设

## 1. 引脚命名规则

厂商页面说明：

- 原始 GPIO 编号主要是 `GPIO0`–`GPIO10`、`GPIO20`、`GPIO21`。
- Arduino 环境中的 `A0`–`A5` 是便于表达模拟功能的映射名称。
- `D*` 名称同样可能是开发板定义产生的映射。
- Arduino 中应选择 `ESP32C3 Dev Module`，并优先按照该板型的 pin mapping 编写代码。

引脚映射图：<https://wiki.nologo.tech/assets/img/esp32/esp32c3supermini/esp32c3foot2.png>

### 1.1 引脚映射表

下表把厂商映射图中的 Arduino 友好名称转换为可供 Agent 直接检索的结构化信息。`A*`、`D*`、`SDA`、`SCL`、`SS`、`MOSI`、`MISO`、`SCK`、`TX`、`RX` 是 Arduino/板卡层面的别名；代码中如无特殊需求，优先使用 `GPIO*` 或数字 GPIO 编号。

| Arduino/板卡标签 | GPIO 编号 | 可用作数字 GPIO | 可用作 PWM | 模拟映射 | 串行/总线别名 | 备注 |
|---|---:|:---:|:---:|---|---|---|
| `A0` / `D0` | 0 | 是 | 是 | `A0` | — | ADC 映射名称；模拟能力以当前板包定义为准 |
| `A1` / `D1` | 1 | 是 | 是 | `A1` | — | ADC 映射名称；模拟能力以当前板包定义为准 |
| `A2` / `D2` | 2 | 是 | 是 | `A2` | `SCK` | 厂商 SPI 示例使用为 SCLK/SCK |
| `A3` / `D3` | 3 | 是 | 是 | `A3` | `MOSI` | 厂商 SPI 示例使用为 MOSI |
| `A4` / `D4` | 4 | 是 | 是 | `A4` | `SCK`（图中别名） | 与 SPI 示例中的 SCLK/GPIO2 不同；以具体接线和代码定义为准 |
| `A5` / `D5` / 5 | 5 | 是 | 是 | `A5` | `MISO` | 引脚页 ADC 示例使用 `A5`；厂商 SPI 别名图标为 MISO |
| `D6` | 6 | 是 | 是 | — | `MOSI`（图中别名） | 厂商 SPI TFT 示例将 GPIO6 用作 DC |
| `D7` | 7 | 是 | 是 | — | `SS` / `CS` | 厂商 SPI TFT 示例将 GPIO7 用作 CS |
| `D8` | 8 | 是 | 是 | — | `SDA`（图中别名） | 板载蓝色 LED；Wi‑Fi LED 示例使用 GPIO8 |
| `D9` | 9 | 是 | 是 | — | `SCL`（图中别名） | I2C 别名；使用前确认当前 Arduino 核心的默认映射 |
| `D10` | 10 | 是 | 是 | — | — | 厂商 SPI TFT 示例将 GPIO10 用作 RST/RES |
| `TX` | 20 | 是 | 是 | — | UART TX | 引脚页文字说明：GPIO20 为 TX；也就是 `Serial1` 的 TX |
| `RX` | 21 | 是 | 是 | — | UART RX | 引脚页文字说明：GPIO21 为 RX；也就是 `Serial1` 的 RX |

> **映射冲突提示**：厂商不同页面/图示对部分 SPI、I2C 别名的表达并不完全一致。例如引脚图中的别名与 TFT 示例的实际 GPIO 分配存在差异。表格保留这些别名，但生成具体项目代码时应以目标外设的接线表、原理图和当前 `ESP32C3 Dev Module` 板包定义为最终依据，不要仅凭 `SDA`/`SCL`/`SCK` 等别名推断 GPIO。

> **ADC 数量提示**：产品参数页写明有 4 个 ADC，引脚页示例使用 `A5`。因此 Agent 不应仅依据 `A0`–`A5` 标签就断言全部六个引脚都具备可用 ADC；请结合当前板包和芯片 ADC 规格确认。


## 2. 能力总览

| 功能 | 说明 |
|---|---|
| GPIO | 11 个数字 I/O 可用于数字输入输出 |
| PWM | 数字 I/O 可用于 PWM，示例使用 GPIO8 驱动板载 LED |
| ADC | 4 个模拟输入，页面示例使用 `A5` |
| UART | 2 个硬件串口路径：USB 串口和 UART 串口 |
| I2C | 可连接 OLED 等器件，示例使用 `SCL`/`SDA` 宏 |
| SPI | 示例使用 GPIO2/3/10/6/7 驱动 ST7735S TFT |
| I2S | 芯片/板卡资料列出的串行接口之一，具体引脚需结合实际映射确认 |

## 3. 数字 GPIO 与板载 LED

```cpp
const int LED_PIN = 8;

void setup() {
  pinMode(LED_PIN, OUTPUT);
}

void loop() {
  digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  delay(1000);
}
```

## 4. PWM

厂商示例使用 `analogWrite()` 让 GPIO8 上的板载 LED 渐变。不同 Arduino-ESP32 版本对 PWM 底层 API 的推荐方式可能不同；如果 `analogWrite()` 行为异常，再改用 `ledcAttach()`/`ledcWrite()` 等当前核心版本 API。

```cpp
const int LED_PIN = 8;

void setup() {
  pinMode(LED_PIN, OUTPUT);
}

void loop() {
  for (int value = 0; value <= 255; value += 5) {
    analogWrite(LED_PIN, value);
    delay(30);
  }
  for (int value = 255; value >= 0; value -= 5) {
    analogWrite(LED_PIN, value);
    delay(30);
  }
}
```

## 5. ADC 模拟输入

厂商示例将电位器连接到 `A5`，读取模拟量并用它控制 LED 闪烁间隔。

```cpp
const int SENSOR_PIN = A5;
const int LED_PIN = 8;

void setup() {
  pinMode(SENSOR_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
}

void loop() {
  int sensorValue = analogRead(SENSOR_PIN);
  int intervalMs = constrain(sensorValue, 10, 2000);

  digitalWrite(LED_PIN, HIGH);
  delay(intervalMs);
  digitalWrite(LED_PIN, LOW);
  delay(intervalMs);
}
```

> ADC 读数范围、衰减、输入电压上限取决于芯片和 Arduino 核心配置。不要将超过允许范围的电压直接接入 ADC。

## 6. 串口

### USB 串口

默认 USB 串口启用。通过 USB Type-C 连接电脑后，可在 Arduino IDE 串口监视器查看输出。

### UART 串口

厂商页面给出的 UART 连接方式：

- GPIO20：TX
- GPIO21：RX
- 需要 USB 串口适配器
- Arduino IDE 中将 **USB CDC On Boot** 设为 `Disabled`

示例：

```cpp
void setup() {
  Serial.begin(115200);  // USB CDC
  Serial1.begin(115200, SERIAL_8N1, 21, 20); // RX=21, TX=20
}

void loop() {
  Serial1.println("hello from UART");
  delay(1000);
}
```

不同 Arduino-ESP32 核心版本对 `Serial1.begin()` 的引脚参数支持可能略有差异；编译失败时应以当前版本 API 为准。

### 软件串口

如需额外串口，可使用 `SoftwareSerial` 类库，但软件串口在高波特率、Wi‑Fi 并发和严格时序场景下可靠性较差。

## 7. I2C：0.96 英寸 OLED

### 接线

| ESP32C3SuperMini | OLED |
|---|---|
| 5V | VCC |
| GND | GND |
| SCL | SCL |
| SDA | SDA |

### Arduino 库

在 **Sketch > Include Library > Manage Libraries** 中安装 `u8g2`。

```cpp
#include <U8g2lib.h>
#include <Wire.h>

U8G2_SSD1306_128X64_NONAME_F_SW_I2C display(
  U8G2_R0,
  /* clock = */ SCL,
  /* data  = */ SDA,
  /* reset = */ U8X8_PIN_NONE
);

void setup() {
  display.begin();
}

void loop() {
  display.clearBuffer();
  display.setFont(u8g2_font_ncenB08_tr);
  display.drawStr(0, 15, "ESP32-C3 SuperMini");
  display.drawStr(0, 32, "I2C OLED OK");
  display.sendBuffer();
  delay(1000);
}
```

如果 OLED 无显示，先确认 I2C 地址、电源电压、SDA/SCL 接线和屏幕控制器型号，再运行 I2C 扫描器。

## 8. SPI：ST7735S TFT

厂商示例的 GPIO 分配：

| TFT 信号 | GPIO |
|---|---:|
| SCLK/SCL | 2 |
| MOSI/SDA | 3 |
| RST/RES | 10 |
| DC | 6 |
| CS | 7 |
| BL | 不接（按实际屏幕需求处理） |

需要安装：

- `Adafruit GFX Library`
- `Adafruit ST7735 and ST7789 Library`

```cpp
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

#define TFT_SCLK 2
#define TFT_MOSI 3
#define TFT_RST  10
#define TFT_DC   6
#define TFT_CS   7

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

void setup() {
  tft.initR(INITR_BLACKTAB);
  tft.fillScreen(ST7735_RED);
  tft.setTextColor(ST7735_YELLOW);
  tft.setTextSize(2);
  tft.setCursor(10, 30);
  tft.print("Hello ST7735!");
}

void loop() {}
```

## 9. 资源冲突检查清单

生成接线或代码前，Agent 应检查：

1. 是否占用了 GPIO8（板载 LED）。
2. 是否同时启用了 USB CDC 和 GPIO20/21 UART，避免串口路径混淆。
3. SPI 的 CS、DC、RST 是否与其他外设冲突。
4. I2C 的 SDA/SCL 是否与目标板型映射一致。
5. ADC 输入是否超过允许电压范围。
6. 外部供电是否与 USB 同时接入。
7. Wi‑Fi 天线区域是否被金属或外壳遮挡。

来源：<https://wiki.nologo.tech/product/esp32/esp32c3/esp32c3supermini/esp32C3SuperMiniFoot.html>
