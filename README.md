# 多平台额度硬件屏

## 默认显示：ChatGPT 7 天 + deepseek + openrouter

默认 `esp32c3_supermini` 固件显示 **ChatGPT 的 7 天窗口**，下面依次显示
DeepSeek 和 OpenRouter 余额，三行铺满整屏，没有单独的底部状态行。例如（示意数值）：

```text
[3d] Δ-4h     95%
deepseek     12.34
openrouter   56.78
```

屏幕显示哪些行、顺序、窗口和刷新间隔都可以配置，存在设备 NVS 的 `rows` 里，
命令行写法是 `update --rows "..."`，默认是 `chatgpt:7d,deepseek,openrouter`：

```text
rows = chatgpt:7d,deepseek,openrouter        # 默认
rows = chatgpt:7d@5m,deepseek@30s            # 每行单独设刷新间隔
rows = chatgpt:5h,chatgpt:7d,openrouter      # 同一来源占两行（5h 有数据时）
rows = chatgpt:7d,deepseek                   # 只留两行，居中排列
```

- 来源：`chatgpt` / `claude` / `deepseek` / `openrouter`，最多 3 行，顺序即行序。
- `chatgpt`、`claude` 可带窗口 `:7d` 或 `:5h`（省略为 `7d`）；余额来源不写窗口。
- 每行可用 `@间隔` 覆盖默认节奏：`30s` / `5m` / `1h`，范围 10 秒～24 小时。
  默认窗口类 10 分钟、余额类 50 秒；同一来源占多行时取其中最短的间隔。
- 没列出的来源**不排队、不抓取、不上屏**，凭据仍保留在设备里。
- 行数少于 3 时整体居中（2 行在 28/44 基线，1 行在 36）。

每一行都有两个视图，各行同步轮换（每个视图停留 2 秒，一轮 4 秒）：

- 视图 1：该行的数据，也就是上面这张图。
- 视图 2：该行的过期时间和状态词，例如 `deepseek  <50s  请求超时`。

只有出错的行才有视图 2：登录失效、网络错误、请求超时等异常出现时，该行在
视图 1 和视图 2 之间切换，其余行保持数据不动；全部正常时只有视图 1，
屏幕不做任何轮换。过期时间是从该行上次成功获取算起的时长，不足一个刷新间隔
时显示 `<10m`（窗口类默认）或 `<50s`（余额默认）；同一来源占多行时按实际抓取
用的最短间隔判定。显示单位按间隔大小选：不到 1 分钟用秒（`<50s` / `55s`）；能被整分钟/
整小时/整天整除的用对应单位（`<10m`、`12m`、`<1h`、`2h`、`<1d`）；不能整除的
（例如 `@90s`）退回秒，避免写出 `<1m` 这种不成立的阈值。

窗口行（ChatGPT / Claude）左侧将重置倒计时和 Δ 组成一组，剩余百分比独立贴右
对齐。ChatGPT 和 Claude 的 5 小时窗口都会解析；要在屏幕上显示，就把
`chatgpt:5h` 或 `claude:5h` 写进行配置。
平台全名默认使用 6×12 字体左对齐，显示 2 个及以上余额时改用 5×8 字体；
金额使用 7×14 字体，不显示单位，统一右对齐。
某个 key 失败不会拖慢整行：只要还有 key 在正常更新，这一行仍按配置的间隔抓取，
不会因为一个长期失效的 key 把整组退避到 15 分钟。
每个平台最多可配置 4 个 API key：同一平台的多个余额按配置顺序用 `+` 连接，
2 个及以上 key 时每个金额保留一位小数（如 `12.0+24.0`），单个 key 仍保留两位
小数（如 `12.34`）。key 较多、整行放不下时金额字体逐级缩小；仍放不下时省略
平台名。未取得余额的位置显示 `--`；某个 key 请求失败时保留它上次成功的值，
并在平台名后加 `*`，其他 key 继续更新。
DeepSeek 只读取 CNY 人民币余额，缺失或无效时保留旧值并标记异常，不回退 USD；OpenRouter 显示
账户 `total_credits - total_usage`，其 `/api/v1/credits` 接口要求 management key。
抓取节奏按行配置：默认窗口类 10 分钟、余额类 50 秒，可用 `@间隔` 逐行覆盖
（见上）。各来源独立排队、各自退避，一个失败不会改变另一个的节奏。抓取流程
本身是串行的：同一次刷新里较慢的请求（例如 OAuth 续期）会推迟其他来源的本次
检查，但不会改变它们各自的截止时间；只有 1px 的 loading 动画跑在独立任务里。
Δ 在 ±1 小时内仍按 5 分钟舍入。

```bash
PLATFORMIO_CORE_DIR="$PWD/.pio-core" pio run -d firmware -e esp32c3_supermini
pnpm --dir cli codex-quota-device login --provider chatgpt
pnpm --dir cli codex-quota-device update --rows "chatgpt:7d,deepseek,openrouter"
```

已有 Wi-Fi、Claude、ChatGPT、DeepSeek、OpenRouter 凭据均保留，切换固件无需
清空配置。注意默认 `rows` 不含 Claude：只跑 `login --provider claude` 的旧设备
升级后，要么把 `claude:7d` 加进 `rows`，要么登录 ChatGPT，否则没有可抓取的行。
`login` 默认 Claude，`--provider chatgpt`（兼容别名 `codex`）走 ChatGPT 的
OAuth；`list` 返回 `rows` 回显当前行配置，以及
`deepseek_balance`、`openrouter_balance` 的余额、币种及过期标记。
多个 key 时 `deepseek_balances`、`openrouter_balances` 逐项返回每个 key 的余额，
汇总对象的 `remaining` 是已取得余额的合计。

## 窗口行显示细节

ESP32-C3 SuperMini + 128×64 I²C OLED 的个人订阅额度显示器。电脑仅用于初次
OAuth 授权、重新登录和配置；正常运行时由 ESP32 自己通过 Wi-Fi 请求用量接口并
续期，不需要电脑常驻程序或中转服务器。

屏幕最多三行，显示哪些行由 `rows` 决定；下面是默认配置（数值仅为布局示意）：

```text
[3d] Δ-4h     95%
deepseek     12.34
openrouter   56.78
```

- 第一行：ChatGPT 7 天窗口，行首方括号显示距重置的剩余时间，右侧是剩余百分比，中间是配速 `Δ`。
- 第二、三行：DeepSeek 和 OpenRouter 余额。
- 换成 `claude:7d` 或 `chatgpt:5h` 时版式相同，只是数据来源/窗口不同。
- 某一行出错时，该行每 2 秒在数据与「过期时间 + 状态词」之间切换一次，
  各行共用同一相位；同一平台多个 key 只有一个失败时，视图 2 显示该 key 的错误，
  视图 1 保留平台名后的 `*`。
- 正在抓取的行会在该行下面显示一条 1px 的进度条（Material 风格的 indeterminate
  高光，从左侧扫到右侧，约 2.4 秒一轮）。它由独立任务驱动，所以 HTTP 等待和
  WiFi 重连期间动画照常走动；哪一行在刷新就只画哪一行，抓完即消失。
- 出错的行会一直显示状态词，不再和数据交替闪烁——没有数据可看时没有第二个视图；
  只有该行已经有数据（或已经有余额）时，才每 2 秒在数据与状态之间轮换一次。
- 时钟还没同步时，窗口行的重置倒计时和配速都显示 `--`，不会把时间戳当成
  天数；NTP 同步后下一帧自动恢复真实值。ESP-IDF 默认不校验证书有效期
  （`CONFIG_MBEDTLS_HAVE_TIME_DATE` 未开启），所以时钟未就绪不影响 HTTPS。

ChatGPT 和 Claude 的 5 小时窗口都会解析，并出现在 `list` 的 `chatgpt_five_hour` /
`five_hour` 里；只有写进 `rows`（`chatgpt:5h` / `claude:5h`）时才在屏幕上占一行。

`剩余百分比 = 100 - utilization`。配速以剩余额度折算的可用时间，减去实际
距重置的时间；正值表示额度消耗比时间进度慢，负值表示快。窗口行都绘制配速
（5 小时窗口只有写进 `rows` 时才显示）。
当 Δ 在 −1 小时到 +1 小时之间（含边界），固定以 5 分钟为步长四舍五入：
`+3m → +5m`、`−3m → −5m`，半档 `±2m30s → ±5m`；舍入为零统一显示
`+0m`，边界显示 `±60m`。超过 1 小时沿用原有小时/天格式。
额度本身不使用进度条（屏幕上唯一的线是抓取时那条 1px loading）。
7 天额度行复用原版额度行布局，整行固定 7×14 字体；百分比与 Δ
间隔 4 像素，Δ 后文字从其起点右移 9 像素绘制，不自动缩小字体。

接口缺失或返回 `null` 的窗口不参与显示：该行画 `[--] … --` 占位，不会整行消失。
百分比存在但重置时间
未知时，行首回退为 `[7d]`，配速显示 `--`。请求失败保留本次
开机以来的最后成功数据及其更新时间；倒计时到零后显示 `[0m]`，百分比仍是最后
采样值，直到下次成功抓取，不会自行假定额度已经恢复。

## 首次使用或从旧固件升级

1. 按 [firmware/README.md](firmware/README.md) 构建并烧录固件。
2. 安装 CLI 依赖。命令名 `codex-quota-device` 保留以兼容已有使用方式。
3. 配置 Wi-Fi；已有配置可以直接沿用。
4. 运行 `login --provider chatgpt`（或 `claude`），在电脑浏览器中为设备单独授权。
   选 `claude` 时还要把 `claude:7d` 写进 `rows`（默认配置里没有 Claude），否则设备
   没有可抓取的行、会显示未配置。

```bash
pnpm --dir cli install
pnpm --dir cli codex-quota-device update \
  --ssid '你的 Wi-Fi' \
  --utc-offset +08:00

pnpm --dir cli codex-quota-device login --provider chatgpt
```

改 SSID 而不传 `--password` 时，CLI 会隐藏输入 Wi-Fi 密码。`update` 只更新
指定参数，不触发 OAuth。刷新间隔现在随行配置走（`--rows "...@10m"`），旧的
`--interval` / `--interval-seconds` 参数仍然会被拒绝（`Unknown option`）；旧固件
保存的间隔值留在 NVS 里但不再影响抓取节奏。新固件收到 `refresh_seconds` /
`refresh_minutes` 时会忽略这两个字段（其它字段照常生效）；只发这两个字段的
旧 CLI 会收到「配置字段无效」。

### 多个 API Key

`--deepseek-key` 和 `--openrouter-key` 可以分别独立更新，都可重复传入或用逗号
分隔，每个平台最多 4 个 key：

```bash
pnpm --dir cli codex-quota-device update \
  --deepseek-key 'sk-...甲' --deepseek-key 'sk-...乙' \
  --openrouter-key 'sk-or-...甲,sk-or-...乙'
```

设备按配置顺序逐个请求 key；某个 key 失败时保留它上次成功的余额，其他 key
继续更新。`list` 的 `deepseek_balances`、`openrouter_balances` 数组逐项给出
每个 key 的 `remaining`、`stale` 和币种（不返回 key 本身）。

Claude 用 `login --provider claude`，ChatGPT 用 `login --provider chatgpt`。
CLI 发起独立的 OAuth 授权，不读取电脑上 Claude Code / Codex CLI 的凭据。浏览器回调地址是 `http://localhost:54545/callback`；无法自动回跳时，
可在 CLI 中粘贴完整回调 URL。可使用 `--no-browser` 自行打开授权页。

获得的访问令牌、刷新令牌和过期时间经 USB 写入设备。CLI 不把它们保存为电脑
文件，也不输出令牌值；诊断仅输出字段名称、大小及脱敏值。登录成功只表示
凭据已写入，是否实际抓取成功以屏幕或 `list` 的状态为准。

旧 Wi-Fi 配置及各平台凭据都保留；屏幕只轮询 `rows` 里列出的来源。没写进
`rows` 的来源（例如默认配置下的 Claude）不再抓取，但凭据还在，改回配置即可
恢复。`erase` 会清除全部设备配置（包括各行凭据）并把 `rows` 恢复为默认。

## 日常操作

```bash
pnpm --dir cli codex-quota-device list
pnpm --dir cli codex-quota-device fetchNow
pnpm --dir cli codex-quota-device erase
```

所有命令自动发现唯一的 Espressif USB 串口；多设备时指定 `--port`。
按设备 `#` 键或执行 `fetchNow` 可请求刷新。手动刷新同样遵守接口限流等待时间
（Claude 的 429 冷却对手动刷新也生效）。

`list` 返回 `rows`（当前行配置的回显，每行都带该行配置的 `@间隔`；同一来源占
多行时实际抓取取其中最短的）、是否已配置、是否需要登录、令牌过期
时间、接口冷却截止时间和两档用量（`chatgpt_five_hour` / `chatgpt_seven_day`、
`five_hour` / `seven_day`），不返回凭据；`usage_provider` 仍固定返回 `claude`
（旧 CLI 兼容字段），行配置以 `rows` 为准。注意串口里的 `used_percent` 是
**已用**百分比，屏幕是**剩余**百分比；`reset_at` 是 Unix 秒。
`chatgpt_status`、`claude_status`、`deepseek_status`、`openrouter_status` 分别
给出每一行在屏幕上的状态（未列进 `rows` 的来源不回显轮询状态）：
`state` 是状态词（与视图 2 右侧一致），`error_detail` 是错误详情，
`age_seconds` 是过期时间（`-1` 表示这一行还没有过成功记录，`0` 才是刚成功过），
`next_refresh_seconds` 是距下次抓取的秒数。
`next_refresh_seconds: 0` 表示该行当前不在队列里（例如凭据待重新登录，或该行
没有写进 `rows`）。
`view_phase` 是按设备时钟算出的当前相位（没有异常行时固定为 1），不是对屏幕内容的
回读：整屏重画约 0.5 秒，刚跨过相位边界时它可能比画面早一步；没有数据的异常行常驻
视图 2 时，相位值仍会交替。`rows_rotating` 表示是否存在异常行（即是否存在视图 2）；
顶层 `next_refresh_seconds` 是当前显示的行里最早的一次抓取。没写进 `rows` 的来源
仍会回显 `*_status`，但状态被清成「未配置」、`age_seconds=-1`。
顶层 `state` 是「第一个异常行的状态」，`last_refresh` 是任一来源最近一次成功
抓取的 Unix 时间，每行自己的新鲜度看 `*_status.age_seconds`。

两套 OAuth 都按同一套保护处理：抓取前检查访问令牌，距过期不足 120 秒时先续期，
用量接口返回 401 时最多续期一次；续期前先把「进行中」状态落盘，成功后把新的
access/refresh/expires 作为单个 NVS 值一次写入。断电或网络中断导致结果不确定时，
设备停止自动重放该刷新令牌，显示「登录失效」，需要再次运行 `login`。

ChatGPT 的凭据同样存在单个 NVS 值里；设备断电/重启时若续期结果不确定，`list` 的
`chatgpt_needs_login` 为 `true`（`chatgpt_pending` 表示「进行中」状态还没确认）。
时钟还没同步时无法换算绝对过期时间，设备用单调时钟把主动续期限流到至少
60 分钟一次；access token 真过期时仍由 401 触发续期（同一轮最多一次）。

Claude 的 429 至少等待 5 分钟，并遵守更长的 `Retry-After`；ChatGPT 没有单独解析
`Retry-After`，429/5xx 走下面同一套通用退避（记为已知限制）。其他暂时失败内部
从 30 秒起翻倍，内部退避上限 15 分钟，但实际等待不短于该行的抓取间隔（默认
窗口 10 分钟、余额 50 秒）；配置的间隔超过 15 分钟时，等待时间以间隔为准。Claude 的冷却截止时间跨重启保留，但只有设备时钟已经同步时才认这个
截止时间——时钟还没就绪时按「没有冷却」处理，避免一个过去的截止时间把重试排到
几天以后。失败只影响出错的那一行，其他行按自己的节奏继续刷新。401/403 持续
失败或续期结果不确定时停止自动续期，`list` 中对应来源的
`claude_needs_login` / `chatgpt_needs_login` 为 `true`。

## 接口与验证边界

Claude：

- 授权：`GET https://claude.ai/oauth/authorize`
- 授权码交换及续期：`POST https://platform.claude.com/v1/oauth/token`
- 订阅用量：`GET https://api.anthropic.com/api/oauth/usage`
- 用量请求携带 Bearer 访问令牌和 `anthropic-beta: oauth-2025-04-20`。

ChatGPT（Codex CLI 的用量接口）：

- 授权及续期：`https://auth.openai.com/oauth/token`（client ID 与 Codex CLI 相同）
- 用量：`GET https://chatgpt.com/backend-api/wham/usage`
- 请求携带 `Authorization: Bearer` 与 `Chatgpt-Account-Id`；响应
  `rate_limit.primary_window` / `secondary_window` 按 `limit_window_seconds`
  区分 5 小时和 7 天窗口。

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
非法响应保留旧数据及 `Retry-After`、行配置语法与布局、ChatGPT 用量窗口映射。
测试不需要登录，也不调用上游服务。
