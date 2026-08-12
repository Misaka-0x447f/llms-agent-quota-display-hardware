# Arduino 入门与故障排查

## 硬件准备

- 1 个 ESP32C3SuperMini
- 1 台电脑
- 1 根支持数据传输的 USB Type-C 数据线

## 初始化步骤

1. USB Type-C 连接开发板和电脑。
2. 安装 Arduino IDE。
3. 添加 ESP32 板包索引：

```text
https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
```

4. 安装 `esp32` 板包。
5. 选择 `ESP32C3 Dev Module`。
6. 选择开发板对应串口。
7. 上传最小 LED 程序。
8. 必要时按一下 RESET 运行程序。

## 常见问题

### Q1：Arduino 找不到串口

按住 BOOT 上电，或：

1. 按住 BOOT。
2. 按下 RESET。
3. 松开 RESET。
4. 松开 BOOT。
5. 重新检查串口。

还需排查：

- USB 线是否只有供电没有数据。
- USB 端口和系统权限是否正常。
- 是否有其他串口程序占用设备。
- 板包和驱动是否已正确安装。

### Q2：上传成功但程序不运行

上传成功后按一下 `RESET`，让芯片重新启动。

### Q3：设备显示为 `JTAG/serial debug unit`

这表示系统识别到了 ESP32-C3 的 USB 调试/串口设备，但 Arduino 端口选择或 USB CDC 配置可能不符合当前使用方式。先确认：

- 选择的是 `ESP32C3 Dev Module`。
- `Tools > Port` 选择了新出现的设备。
- 需要 USB 串口时，将 `USB CDC On Boot` 设为 `Enabled`。
- 仍无法使用时，进入下载模式后重新枚举设备。

厂商补充方案：<https://chat.nologo.tech/d/72/3>

### Q4：Arduino 串口监视器没有输出

将 **Tools > USB CDC On Boot** 设为 `Enabled`，重新编译、上传并打开 `115200 baud` 串口监视器。

如果使用 GPIO20/21 的 UART，则应将 USB CDC On Boot 设为 `Disabled`，并使用外部 USB-UART 适配器。

## 代码生成规范

- 所有网络凭据使用环境变量、配置文件或烧录时注入，不要硬编码真实密码。
- 网络连接必须有超时、重试上限和失败状态输出。
- 读取 ADC 后应做范围约束，避免直接把原始读数作为长时间阻塞延迟。
- 对 HTTP 服务限制请求规模，并显式关闭客户端连接。
- 涉及供电、天线或外设电压时，先给出接线前检查项。
- 生成 GPIO 代码时明确写出 GPIO 编号，避免 `A*`、`D*` 映射歧义。

## 延伸资料

- ESP32 Arduino 教程：<https://docs.geeksman.com/esp32/#%E7%9B%AE%E5%BD%95-esp32arduino>
- ESP32 MicroPython 教程：<https://docs.geeksman.com/esp32/#%E7%9B%AE%E5%BD%95-micropython>
- ESP32-C3 官方 ESP-IDF：<https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32c3/get-started/index.html>
- 厂商论坛：<https://chat.nologo.tech/>
- 厂商原始页面：<https://wiki.nologo.tech/product/esp32/esp32c3/esp32c3supermini/esp32C3SuperMini.html>
