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

串口为 115200 baud。固件使用 NVS 明文保存 Wi-Fi、ChatGPT（Codex）、Claude 和
各平台余额凭据；当前阶段没有启用 ESP32 Flash Encryption。默认显示 ChatGPT 7 天
额度、DeepSeek 和 OpenRouter 余额；Claude 凭据存入独立的 `claude_auth` NVS 值，
不复用旧令牌。
每个平台最多 4 个 API key，多个余额在屏幕上用 `+` 拼接，2 个及以上时每个
金额保留一位小数；单个 key 仍保留两位小数。

默认固件使用 `pnpm --dir cli codex-quota-device login --provider chatgpt` 授权；
`claude` 仍可用，只是默认不显示。

正在抓取的行会在该行下面显示 1px 的进度条，线在行基线 +2（ink 下界见
`scripts/font_ink_probe.c`，实测本行最低像素是基线 +1）。WiFi 重连阶段（最长
15 秒）本轮到期的行都会亮线，联网成功后切成只点亮正在请求的那一行。软件 I²C
实测约 1.4 KB/s：一整页 128 B 约 90 ms、按此折算整屏约 0.9 秒；另有一次整屏重画
阻塞主循环约 0.45–0.5 秒的实测，两者口径不同、差异未查清。所以动画放在独立任务里，
每帧只重发高光变化的两端 tile（单来源 15～23 B，实测 17～26 fps；起步或整屏
重画之后整段补一次）。整屏重画仍沿用既有节奏（每次到期刷新之后都会做一次，不论数值是否
变化）；动画只是在两次整屏重画之间补上实时反馈。只有真正发起抓取的行才置位
`fetchingMask`，排队中的行没有线。

电脑退出后，ESP32 自己请求用量及续期。屏幕显示哪些行、顺序、窗口和刷新间隔
由 NVS 的 `rows` 决定，默认 `chatgpt:7d,deepseek,openrouter`（最多 3 行，
未列出的来源不抓取也不上屏）。没有单独的底部状态行，出错时该行每 2 秒在数据
与「过期时间 + 状态词」之间切换，各行共用同一相位；行数少于 3 时整体居中。
默认窗口类每 10 分钟抓一次、余额每 50 秒抓一次，每行可用 `@30s` / `@5m` 覆盖，
同一来源占多行时取最短；各来源独立排队。5 小时窗口只在写进 `rows` 时才显示。
ChatGPT 与 Claude 的凭据都按单个 NVS 值保存，续期前先写「进行中」状态。
首次授权与设备续期都成功后，才算验证了独立运行能力。`login` 成功仅确认串口
写入，不确认上游接口可用。

## 时间同步

固件用 SNTP 常驻同步设备时钟，NTP 源固定为国内可达的 `ntp.aliyun.com`、
`ntp1.aliyun.com`、`time.tencent.com`：只在尚未取到时间时启动一次，不阻塞
抓取流程，之后每小时校正一次走时漂移。时钟只依赖 NTP，不用接口返回的时间校准。
时钟未就绪时，窗口行的重置倒计时和配速都显示 `--`；其余内容照常显示，
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
