# ESP32-C3 SuperMini

> 面向 Agent 的硬件资料汇总。适用于代码生成、硬件连接推理、Arduino/ESP-IDF/MicroPython 项目初始化与故障排查。

## 0. 快速结论

- **板卡**：ESP32C3SuperMini
- **核心芯片**：Espressif ESP32-C3FN4
- **CPU**：32 位单核 RISC-V，最高 160 MHz，带单精度浮点单元
- **无线**：2.4 GHz Wi‑Fi（802.11 b/g/n）+ Bluetooth 5 LE
- **板载 Flash**：4 MB
- **尺寸**：约 22.52 × 18 mm
- **板载 LED**：GPIO8
- **主要外设**：11 个 GPIO/PWM、4 个 ADC、UART、I2C、SPI、I2S
- **供电**：USB Type-C，或外部 3.3–6 V 接入 5V/GND
- **默认 Arduino 板型**：`ESP32C3 Dev Module`

## 1. 适用场景

- 小型 Wi‑Fi 传感器、状态上报与局域网控制器
- 低功耗物联网节点
- 无线可穿戴设备
- 小型 OLED、SPI TFT、继电器、LED、按键等外设控制
- Arduino 原型验证、ESP-IDF 固件开发、MicroPython 实验

## 2. 关键硬件参数

| 项目 | 参数 |
|---|---|
| SoC | ESP32-C3FN4 |
| CPU | 32-bit RISC-V 单核，最高 160 MHz |
| SRAM | 400 KB |
| ROM | 384 KB |
| Flash | 板载 4 MB |
| Wi‑Fi | IEEE 802.11 b/g/n，2.4 GHz |
| Wi‑Fi 模式 | Station、SoftAP、SoftAP+Station、混杂模式 |
| 蓝牙 | Bluetooth 5 LE |
| 深度睡眠 | 约 43 µA（以厂商页面为准，实际值与板级电路和测量条件有关） |
| ADC | 4 路模拟输入 |
| PWM | 11 个数字 I/O 可用于 PWM |
| 接口 | 2×UART、1×I2C、1×SPI、1×I2S |
| 安全 | AES-128/256、Hash、RSA、HMAC、数字签名、安全启动硬件加速 |
| LED | 蓝色，GPIO8 |
| USB | USB Type-C |
| 尺寸 | 22.52 × 18 mm |

## 3. 供电与安全

### USB 供电

使用 USB Type-C 数据线连接电脑。注意：有些 Type-C 线只有供电能力，没有数据线芯，无法用于烧录和串口通信。

### 外部供电

- 正极接板上的 `5V` 位置
- 负极接 `GND`
- 支持外部 3.3–6 V 电源
- 外部供电与 USB **二选一，不要同时连接**
- 接线前确认正负极，避免短路、损坏电池或开发板

## 4. 天线

板卡默认配有外部天线以改善无线信号。若修改或接入外部天线，必须按照板卡射频区域和原理图的天线连接方式操作；不要在不确认射频匹配网络的情况下直接焊接。

## 5. Agent 使用时的默认假设

除非项目上下文另有说明，生成 Arduino 示例时使用：

```cpp
// Arduino Board: ESP32C3 Dev Module
// Board LED: GPIO8
// Serial monitor baud rate: 115200
```

生成引脚代码时优先使用 `GPIO0`–`GPIO10`、`GPIO20`、`GPIO21` 这类 GPIO 编号。`A0`–`A5` 和 `D*` 名称可能来自 Arduino 板型映射，不应与芯片原始 GPIO 编号混用。

## 6. 工具链选择

### Arduino IDE

1. 安装最新 Arduino IDE。
2. 在 **File > Preferences** 中加入 ESP32 板包索引：

```text
https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
```

3. 在 **Tools > Board > Boards Manager** 搜索并安装 `esp32`。
4. 选择 **ESP32 Arduino > ESP32C3 Dev Module**。
5. 在 **Tools > Port** 选择开发板串口。
6. 若 Arduino 串口监视器没有输出，将 **USB CDC On Boot** 设置为 `Enabled`。

### MicroPython

- 固件下载：<https://micropython.org/download/esp32c3-usb/>
- 厂商刷写教程：<https://chat.nologo.tech/d/75>

### ESP-IDF

官方 ESP32-C3 入门资料：<https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32c3/get-started/index.html>

## 7. 烧录与启动

### 正常烧录

通过 USB Type-C 连接开发板，选择正确板型和串口后上传。

### 无法识别串口时进入下载模式

推荐顺序：

1. 按住 `BOOT`。
2. 按一下 `RESET`。
3. 松开 `RESET`。
4. 松开 `BOOT`。
5. 重新观察串口设备并开始烧录。

部分环境需要每次重新进入下载模式；如果串口短暂出现后消失，可通过系统设备连接提示音确认枚举状态。

### 上传后程序不运行

上传成功后按一下 `RESET`，让芯片重新启动并执行新程序。

## 8. 最小验板程序

```cpp
const int LED_PIN = 8;

void setup() {
  pinMode(LED_PIN, OUTPUT);
}

void loop() {
  digitalWrite(LED_PIN, HIGH);
  delay(1000);
  digitalWrite(LED_PIN, LOW);
  delay(1000);
}
```

> 厂商示例注释中对 LED 高低电平的“亮/灭”描述存在不一致。若实际现象相反，只需交换 `HIGH` 和 `LOW` 的语义即可；代码本身仍能验证 GPIO8 是否正常工作。

## 9. 资料索引

- [Wi‑Fi 使用](./wifi.md)
- [引脚与外设](./pins-and-peripherals.md)（包含引脚映射图链接和结构化引脚表）
- [Arduino 入门与常见故障](./arduino-getting-started.md)
- [厂商原始入门页](https://wiki.nologo.tech/product/esp32/esp32c3/esp32c3supermini/esp32C3SuperMini.html)
- [厂商 Wi‑Fi 页](https://wiki.nologo.tech/product/esp32/esp32c3/esp32c3supermini/esp32C3SuperMiniWifi.html)
- [厂商引脚页](https://wiki.nologo.tech/product/esp32/esp32c3/esp32c3supermini/esp32C3SuperMiniFoot.html)

## 10. 官方页面中的注意事项

- 页面写作“支持 UART、I2C 和 SPI 等四种串行接口”，引脚页明确列出第四种为 I2S。
- 页面部分示例代码带有教学性质，生成新项目时应补充超时处理、状态码检查、输入校验和敏感配置隔离。
- Wi‑Fi 示例中的 SSID 和密码必须替换为项目配置，不要硬编码真实凭据到公开代码仓库。
