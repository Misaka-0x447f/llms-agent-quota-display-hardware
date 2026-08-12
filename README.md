# Codex 额度硬件屏

ESP32-C3 SuperMini + 128×64 I²C OLED 的个人额度显示器。电脑只在初次登录或
OAuth 失效时运行 CLI；正常工作时由 ESP32-C3 自行连接 Wi-Fi、刷新 token，并
直接请求 Codex 用量接口，不需要常驻 bridge。

屏幕采用三行紧凑布局：

```text
[4h]  68% Δ+1h12m
[3d]  40% Δ+4h
更新 5m   正在刷新
```

其中：

- 5 小时额度（接口不返回时自动隐藏）；
- 7 天额度；
- 下次 reset 的剩余时间、剩余百分比和 `Δ±时间` 配速；
- 刷新间隔及右对齐的状态文本；没有状态时右侧为空；
- 中文短错误：未配置、网络异常、登录失效、请求失败、数据异常。

刷新间隔只接受 1～1440 分钟。底行左侧显示距上次成功刷新的时间：一分钟内
显示 `<1m 前`，之后按分钟向下取整显示 `Nm 前`，没有成功记录时显示 `--`。
若已有凭据但持久化的间隔越界，固件不会回退到默认值，而是显示
`--   配置无效` 并停止刷新。

配速计算复刻指定 gist：`remaining = 100 - used_percent`，
`expected = time_left / window * 100`。5h 窗口头 3 分钟隐藏 Δ；7d 仅在消耗
快于时间配速时显示负 Δ。屏幕不使用进度条。

## 首次使用

1. 按 [firmware/README.md](firmware/README.md) 构建并烧录。
2. 松开 `*`/BOOT 后复位，屏幕应显示“未配置”。
3. 安装并运行 Node.js CLI（密码不写进 shell history 时，不传 `--password`）：

```bash
pnpm --dir cli install
pnpm --dir cli codex-quota-device set \
  --ssid '你的 Wi-Fi' \
  --interval 5 \
  --utc-offset +08:00

pnpm --dir cli codex-quota-device login
```

`set` 只写入 Wi-Fi、刷新间隔和时区，不会触发 OAuth。`login` 才会打开
`https://auth.openai.com/oauth/authorize`，并临时监听
`http://localhost:1455/auth/callback` 接收浏览器回调；登录成功后把所需凭据
经 USB 串口写入 ESP32-C3。token 不会写入电脑文件。若浏览器无法自动回跳，CLI
会提示粘贴完整 callback URL。

常用命令：

```bash
pnpm --dir cli codex-quota-device list
pnpm --dir cli codex-quota-device fetchNow
pnpm --dir cli codex-quota-device erase
```

`list` 查看设备状态，`fetchNow` 要求设备立即抓取一次额度，`erase` 清除设备本地配置。
CLI 的 `fetchNow` 会映射为固件协议中的 `refresh`；`login` 需要配合本次新增的
固件 `login` 协议一起烧录。

所有命令默认自动发现唯一的 Espressif USB 串口；有多个候选设备时才需要传
`--port /dev/serial/by-id/...`。

## 协议来源与风险

OAuth 浏览器回调、token refresh 和 account ID 提取按 CLIProxyAPI 当前实现兼容：

- `GET https://auth.openai.com/oauth/authorize`
- `POST https://auth.openai.com/oauth/token`
- `GET https://chatgpt.com/backend-api/wham/usage`

这些不是承诺长期稳定的公开用量 API；上游字段或鉴权策略改变时需要同步更新固件。

## 验证

```bash
pnpm --dir cli check
PLATFORMIO_CORE_DIR="$PWD/.pio-core" pio run -d firmware
```
