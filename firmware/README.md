# ESP32-C3 Codex 额度屏固件

## 直插映射

| 模块 | ESP32-C3 |
|---|---:|
| GND | GND |
| VCC / 3V3 | 3V3 |
| SCL | GPIO9 / SCL |
| SDA | GPIO8 |
| K4 `*` | GPIO3 |
| K3 `#` | GPIO2 |
| K2 下 | GPIO1 |
| K1 上 | GPIO0 |

`#` 用于手动刷新。OLED 直接使用开发板的固定 `GND`/`3V3` 电源脚，固件
不得把 GPIO5/6 当作供电输出。

显示驱动为已经实物确认的 128×64 SSD1315，使用已通过验屏和按键测试的
软件 I²C；地址固定为实物确认的 `0x3C`，启动时不进行阻塞式地址扫描。

## 构建与烧录

```bash
PLATFORMIO_CORE_DIR="$PWD/.pio-core" pio run -d firmware
PLATFORMIO_CORE_DIR="$PWD/.pio-core" pio run -d firmware -t upload \
  --upload-port /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_*-if00
```

串口为 115200 baud。固件使用 NVS 明文保存 Wi-Fi、access token、refresh token
和 account ID；当前阶段没有启用 ESP32 Flash Encryption。

HTTPS 使用 `data/cert/x509_crt_bundle.bin` 中嵌入的根证书集合校验服务端证书，
没有使用 `setInsecure()`。

## 中文字形

构建前脚本会扫描 `src/main.cpp` 中的 UTF-8 中文字符串，从 U8g2 的
`u8g2_font_wqy12_t_gb2312` 提取实际用到的 12×12 字形，并自动更新
`src/ui_glyphs.h`。固件不会链接约 198 KiB 的完整中文字库。

也可以手动同步或在 CI 中只做一致性检查：

```bash
cd firmware
python3 scripts/sync_ui_glyphs.py
python3 scripts/sync_ui_glyphs.py --check
```
