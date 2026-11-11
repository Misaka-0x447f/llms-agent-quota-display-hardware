# ESP32-C3 多平台额度屏固件

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
PLATFORMIO_CORE_DIR="$PWD/.pio-core" pio run -d firmware -e esp32c3_supermini
PLATFORMIO_CORE_DIR="$PWD/.pio-core" pio run -d firmware -e esp32c3_supermini -t upload \
  --upload-port /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_*-if00
```

串口为 115200 baud。固件使用 NVS 明文保存 Wi-Fi 和 Claude 凭据；当前阶段
没有启用 ESP32 Flash Encryption。默认显示 Claude 7 天额度、DeepSeek 和
OpenRouter 余额；Claude 凭据存入独立的 `claude_auth` NVS 值，不复用旧令牌。
每个平台最多 4 个 API key，多个余额在屏幕上用 `+` 拼接，2 个及以上时每个
金额保留一位小数；单个 key 仍保留两位小数。

默认固件使用 `pnpm --dir cli codex-quota-device login --provider claude` 授权。

电脑退出后，ESP32 自己请求用量及续期。屏幕三行依次是 Claude 7 天窗口、
DeepSeek 余额、OpenRouter 余额；没有单独的底部状态行，出错时该行每 4 秒在数据
与「过期时间 + 状态词」之间切换，三行共用同一相位。三行沿用原来的 16px 行距，
上下各留出半行左右的空闲，观感与加底部状态行之前一致。Claude 用量每 10 分钟抓一次，
余额每 50 秒抓一次，各自独立排队。Claude 5 小时窗口只解析不绘制。
首次授权与设备续期都成功后，才算验证了独立运行能力。`login` 成功仅确认串口
写入，不确认上游接口可用。

## 时间同步

固件用 SNTP 常驻同步设备时钟，NTP 源固定为国内可达的 `ntp.aliyun.com`、
`ntp1.aliyun.com`、`time.tencent.com`：只在尚未取到时间时启动一次，不阻塞
抓取流程，之后每小时校正一次走时漂移。时钟只依赖 NTP，不用接口返回的时间校准。
时钟未就绪时，Claude 行的重置倒计时和配速都显示 `--`；其余内容照常显示，
HTTPS 也不受影响（ESP-IDF 默认不校验证书有效期）。服务端 429 的冷却截止时间
是 Unix 时间，时钟未就绪时固件不认这个截止时间，避免冷启动把重试排到几天后。

NVS 在续期前记录进行中状态，成功后一次写入新令牌对及过期时间；如果重启时
仍处于进行中状态，设备要求重新登录，避免重复使用可能已经被消费的刷新令牌。

HTTPS 使用 `data/cert/x509_crt_bundle.bin` 中嵌入的根证书集合校验服务端证书，
没有使用 `setInsecure()`。

## 中文字形

构建前脚本会扫描 `src/main.cpp` 和 `src/row_view.h` 中的 UTF-8 中文字符串，
从 U8g2 的 `u8g2_font_wqy12_t_gb2312` 提取实际用到的 12×12 字形，并自动更新
`src/ui_glyphs.h`。新增含界面文案的源文件时要把它加进脚本的扫描列表，否则
缺字形的地方会画成空白。固件不会链接约 198 KiB 的完整中文字库。

也可以手动同步或在 CI 中只做一致性检查：

```bash
cd firmware
python3 scripts/sync_ui_glyphs.py
python3 scripts/sync_ui_glyphs.py --check
```
