# Claude 额度硬件屏

ESP32-C3 SuperMini + 128×64 I²C OLED 的个人订阅额度显示器。电脑仅用于初次
OAuth 授权、重新登录和配置；正常运行时由 ESP32 自己通过 Wi-Fi 请求 Claude
用量接口并续期，不需要电脑常驻程序或中转服务器。

屏幕只显示 Claude，固定三行（以下数值仅为布局示意）：

```text
[3h]  80% Δ+1h
[3d]  40% Δ-4h
5m 前    正在刷新
```

- 第一行：5 小时窗口，行首方括号显示距重置的剩余时间，随后是剩余百分比和配速 `Δ`。
- 第二行：7 天窗口，同样以距重置的剩余时间开头；行首不是固定的 `5h` / `7d` 标签。
- 第三行：距最后一次成功获取用量的时间，以及当前状态或中文错误。

`剩余百分比 = 100 - utilization`。配速以剩余额度折算的可用时间，减去实际
距重置的时间；正值表示额度消耗比时间进度慢，负值表示快。两档都保留配速。
屏幕不使用进度条。两行复用原版额度行布局，整行固定 7×14 字体；百分比与 Δ
间隔 4 像素，Δ 后文字从其起点右移 9 像素绘制，不自动缩小字体。

接口缺失或返回 `null` 的窗口沿用原版行为，整行隐藏。百分比存在但重置时间
未知时，行首回退为该窗口的 `[5h]` 或 `[7d]`，配速显示 `--`。请求失败保留本次
开机以来的最后成功数据及其更新时间；倒计时到零后显示 `[0m]`，百分比仍是最后
采样值，直到下次成功抓取，不会自行假定额度已经恢复。

## 首次使用或从旧固件升级

1. 按 [firmware/README.md](firmware/README.md) 构建并烧录 Claude 固件。
2. 安装 CLI 依赖。命令名 `codex-quota-device` 保留以兼容已有使用方式。
3. 配置 Wi-Fi 和刷新间隔；已有配置可以直接沿用。
4. 运行 `login`，在电脑浏览器中为设备单独授权 Claude。

```bash
pnpm --dir cli install
pnpm --dir cli codex-quota-device update \
  --ssid '你的 Wi-Fi' \
  --interval 5 \
  --utc-offset +08:00

pnpm --dir cli codex-quota-device login
```

改 SSID 而不传 `--password` 时，CLI 会隐藏输入 Wi-Fi 密码。`update` 只更新
指定参数，不触发 OAuth。刷新间隔支持 1～1440 分钟；无效的持久化间隔会停止
刷新并显示“配置无效”，不会静默回退。建议先使用默认 5 分钟。

`login` 默认选择 Claude（也可显式传 `--provider claude`）。CLI 先检查设备
是否支持 Claude，再发起独立的 PKCE OAuth 授权，不读取电脑上 Claude Code 的
凭据。浏览器回调地址是 `http://localhost:54545/callback`；无法自动回跳时，
可在 CLI 中粘贴完整回调 URL。可使用 `--no-browser` 自行打开授权页。

获得的访问令牌、刷新令牌和过期时间经 USB 写入设备。CLI 不把它们保存为电脑
文件，也不输出令牌值；诊断仅输出字段名称、大小及脱敏值。登录成功只表示
凭据已写入，是否实际抓取成功以屏幕或 `list` 的状态为准。

旧 Wi-Fi 配置及旧平台凭据保留，但本版本只轮询和显示 Claude。旧 Codex 授权
仍可通过 `login --provider codex` 写入；旧余额 key 配置选项也保留兼容，不再
影响屏幕。`erase` 会清除全部设备配置，包括旧平台凭据。

## 日常操作

```bash
pnpm --dir cli codex-quota-device list
pnpm --dir cli codex-quota-device fetchNow
pnpm --dir cli codex-quota-device update --interval 10
pnpm --dir cli codex-quota-device erase
```

所有命令自动发现唯一的 Espressif USB 串口；多设备时指定 `--port`。
按设备 `#` 键或执行 `fetchNow` 可请求刷新。手动刷新同样遵守接口限流等待时间。

`list` 返回 `usage_provider: "claude"`、是否已配置、是否需要登录、令牌过期
时间、接口冷却截止时间和两档用量，不返回凭据。注意串口中的 `used_percent`
是**已用**百分比，屏幕是**剩余**百分比；`reset_at` 是 Unix 秒。

设备在抓取前检查访问令牌，距过期不足 120 秒时先续期；用量接口返回 401 时
最多续期一次并重试。刷新间隔很长时，可在访问令牌已经过期后用刷新令牌续期。
新的令牌对、过期时间和续期状态通过单个 NVS 值一起保存，避免混用不同代凭据。

续期前先持久化“进行中”状态。如果断电或网络中断导致续期结果不确定，设备
会停止自动重放该刷新令牌，显示“登录失效”，需要再次运行 `login`。这避免
把可能已被上游消费的刷新令牌反复发送，但不能保证任何断电时刻都免重新登录。

429 至少等待 5 分钟，并遵守更长的 `Retry-After`；其他暂时失败从 30 秒开始
指数退避，最长 15 分钟。接口冷却截止时间跨重启保留。401/403 持续失败或续期
结果不确定时停止自动续期，`list` 中 `claude_needs_login` 为 `true`。

## 接口与验证边界

- 授权：`GET https://claude.ai/oauth/authorize`
- 授权码交换及续期：`POST https://platform.claude.com/v1/oauth/token`
- 订阅用量：`GET https://api.anthropic.com/api/oauth/usage`
- 用量请求携带 Bearer 访问令牌和 `anthropic-beta: oauth-2025-04-20`。

授权参数参考 [CLIProxyAPI 的 Claude 实现](https://github.com/router-for-me/CLIProxyAPI/blob/main/internal/auth/claude/anthropic_auth.go)，
用量结构及表单续期参考 [CodexBar 的 Claude 实现](https://github.com/steipete/CodexBar/blob/main/docs/claude.md)。
使用 Claude Code 的公共 OAuth client ID，授权范围包含用量接口所需的
`user:profile`；它不是本仓库注册的独立 OAuth 应用。`claude setup-token`
生成的推理令牌不能替代本流程。

这些客户端接口没有本项目可依赖的长期兼容性保证。上游 OAuth 防护是否接受
ESP32 的 TLS 客户端，需要实际设备授权后验证；电脑端成功不等于设备续期成功。
板上验收应覆盖：首次抓取、设备自行续期、用新令牌抓取、正常断电重启后继续抓取。

## 开发验证

```bash
pnpm --dir cli check
PLATFORMIO_CORE_DIR="$PWD/.pio-core" pio test -d firmware -e protocol_native
PLATFORMIO_CORE_DIR="$PWD/.pio-core" pio run -d firmware -e esp32c3_supermini
python3 firmware/scripts/sync_ui_glyphs.py --check
```

原生协议测试在电脑上执行与固件相同的解析代码，覆盖 UTC/时区/闰日、可选窗口、
非法响应保留旧数据及 `Retry-After`。测试不需要登录，也不调用 Claude 服务。
