# 任务记录：三行双视图与固定刷新间隔

## 当前阶段与下一步

- 阶段 3（实施与验证）：三轮审查完成，严重问题全部修复并上机验证；轮末判断为**可以交付**。
- 下一步：请用户确认交付（提交到 `main` 并推送）。第 3 轮之后只做了「视图 2 排版抽成纯函数 + 两条不变量
  测试」这一处行为等价的改动，已用测试与变异验证，但未再单独开一轮审查，交付说明里如实标注。

## 目标（已与用户定稿）

1. 去掉屏幕最下方的状态行，把空间还给三行数据。
2. 每一行做两个视图轮换，各行之间同步切换（同一时刻所有行处于同一视图号）：
   - 视图 1：该行数据（与现在一致）。
   - 视图 2：该行的过期时间 + 状态词，即原来底部状态行的信息。
   - 没有异常的行不存在视图 2（等价于两个视图相同，不轮换）。
   - 视图 2 的过期时间与状态词沿用原状态行的左右布局：左为过期时间，右为状态词。
3. 刷新频率固定：Claude 用量 10 分钟，DeepSeek / OpenRouter 余额 50 秒，各自独立排队。
4. 过期时间显示规则：不足一个刷新间隔时显示 `<间隔`（Claude `<10m`，余额 50 秒折算 `<1m`），
   否则显示向下取整的分钟数（如 `12m`）；没有成功记录或时间不可用时显示 `--`。
5. CLI 与固件协议删除 `--interval` / `--interval-seconds` / `refresh_seconds` / `refresh_minutes`
   （用户已确认）。

## 已确认的产品规则

- 「空闲 / 正常 / 刷新中 / 未配置」都不算异常，不产生视图 2。
- 首次抓取完成前没有异常，不轮换。
- 同一行有多个 key、部分失败时：该行仍算异常，视图 2 显示第一个失败 key 的状态，视图 1 保留 `*`。
- 视图相位每 4 秒切换一次（`kViewHoldMs`），可调。
- 布局保持原来的观感：三行仍相隔 16px，上下各留出半行左右的空闲
  （行基线 20 / 36 / 52，用户上机看过后又要求整体上移 2px）。
- 时钟只依赖 NTP；不拿 API 返回的时间校准设备时钟。NTP 未就绪时剩余时间显示 `--`，不显示假天数。

## 必须保留的行为

- 三行分别是 Claude 7 天窗口、DeepSeek 余额、OpenRouter 余额；数据排版（7×14 金额、+ 拼接、
  平台名逐级缩小、`*` 陈旧标记）不变。
- 串口协议命令名（`status` / `configure` / `claude_login` / `refresh` / `erase`）与既有字段
  （`state`、`error_detail`、`last_refresh`、`deepseek_*` / `openrouter_*`）保持可用。
- Claude 的 token 轮换、429 冷却、`pending` 凭据失效保护不变。
- 按键 `#` 手动刷新、`fetchNow` 手动刷新语义不变（所有来源一起尝试）。

## 验收清单

| # | 预期来源 | 入口与条件 | 可观察结果 | 验证方法 |
| --- | --- | --- | --- | --- |
| A1 | 本次需求 | 屏幕（正常状态，三行都有数据） | 底部无旧状态行；三行铺满；无任何行显示视图 2，也不轮换 | 上机观察 + 截图 |
| A2 | 本次需求 | 屏幕（制造单行失败：拔网 / 错误 key） | 该行每 4 秒在数据与「过期时间 + 状态词」之间切换，另外两行不动且相位同步 | 上机观察 + 截图 |
| A3 | 本次需求 | 屏幕（Claude 登录失效） | Claude 行视图 2 显示 `<10m` 后随时间增长为 `10m/11m…` 以及「登录失效」 | 上机观察 + 截图 |
| A4 | 本次需求 | 屏幕（余额行出错） | 余额行视图 2 过期时间不足 50 秒显示 `<1m`，状态词为第一个失败 key 的错误，视图 1 保留 `*` | 上机观察 + 原生测试 |
| B1 | 本次需求 | 设备运行 5 分钟以上 | Claude 只每 10 分钟抓一次；余额每 50 秒一次；`status` 的分行 `next_refresh_seconds` 与之一致 | CLI `list` 周期采样 + 串口日志 |
| B2 | 保留行为 | `fetchNow` | 立即刷新所有来源，返回 ok | CLI `fetchNow` |
| C1 | 本次需求 | `update` 不带 `--interval` | 正常写入；带 `--interval` 时报未知参数 | CLI 单测 + 实机 |
| C2 | 保留行为 | `login` / `erase` / 按键 | 行为不变 | 手动 + 单测 |

## 事实与证据

- 迁移前代码：`git rev-parse HEAD` = 29ddedb。
- 并行执行检查（2026-11-11T22:21+08:00）：工作区只有本任务的改动，`git log` 最近提交为
  上游历史，无其他 agent 进程占用该仓库；可见范围限于本机进程与 git 状态。
- 设备：ESP32-C3 SuperMini，串口 `/dev/ttyACM0`，局域网内网地址（不写入仓库细节）。
- NTP 根因（上一阶段已确认）：Arduino 核心 `configTime()` 会 `sntp_stop()` + `sntp_init()`，
  每轮重置；且 `time.google.com` 在境内超时。已改为国内源 + SNTP 常驻只启一次。

## 已知取舍与风险

- 设备级 `configured()` 仍是「有 Wi-Fi 且有 Claude 凭据」，只有余额 key 没有 Claude 登录时
  整个刷新循环不启动（既有行为，本次不改）。
- 视图 2 没有「当前是第几个视图」的角标：右上角放不下且会压住第一行。
- `period_s` 旧值仍留在 NVS，不再读写；不主动清除。

## 审查与验证记录

### 构建与测试（2026-11-11 22:2x）

- `pio test -d firmware -e protocol_native`：32 个用例全部通过（新增 test_row_view 5 项、
  test_freshness_format 3 项、test_refresh_plan 3 项；test_quota_behavior 删掉了已废弃的
  间隔配置用例，保留 Δ 舍入用例）。
- `pio run -d firmware -e esp32c3_supermini`：SUCCESS，字形同步 103 个（「前」随旧状态行
  文案一起消失），Flash 79.9%。
- `pnpm --dir cli run check`：typecheck + 20 个测试全部通过。
- 烧录：`pio run -t upload --upload-port /dev/ttyACM0`，Hash verified。

### 上机验证（设备 192.168.50.107）

| 验收项 | 结果 | 证据 |
| --- | --- | --- |
| A1 无异常不轮换 | 通过 | 22:29–22:32 连续 7 次采样 `rows_rotating=false`、`state=""` |
| A2 单行异常才轮换 | 通过 | 22:32:30 OpenRouter 真实 `TCP被拒绝`：`rows_rotating=true`，Claude/DeepSeek 状态仍为空，各自节奏不变 |
| A2 相位同步交替 | 通过 | 22:33:00–22:33:16 每 3 秒采样，`view_phase` 1→2→1→2 交替（4 秒相位） |
| A4 余额过期时间与退避 | 通过 | 失败期间 `age_seconds` 97→113 递增、`next_refresh_seconds` 递减到底后 50 秒重试；22:34:10 恢复为 `state=""`、余额 19.4126→19.3786 更新 |
| B1 固定节奏 | 通过 | Claude `next_refresh_seconds` 恒等于 600-age（594/571/547/…/321）；余额恒等于 50-age |
| B2 手动刷新 | 通过 | `fetchNow` 后 Claude 从无数据恢复为 seven_day used 73%（剩余 27%） |
| C1 CLI 删除间隔参数 | 通过 | `update --interval 5` 报 `Unknown option '--interval'`；`update --utc-offset +08:00` 正常写入 |
| A1/A2/A3 像素级外观 | 未验证 | 无摄像头，OLED 画面需用户目视确认 |

- Claude 冷却：开机时仍带着上一轮持久化的 429 冷却（`claude_retry_at`），首次自动抓取
  按 10 分钟退避；手动 `fetchNow` 在冷却结束后立即成功。符合「退避不短于间隔」的策略。
- alint：`alint config inspect` 对 main.cpp / row_view.h / cli.ts / README.md 的 `rules:`
  均为空，仓库没有适用规则，按规则跳过 `alint --dirty`。

### 第 2 轮代码审查（Claude 系列 subagent：subagent_claude_code，HEAD 29ddedb + 工作区）

| # | 意见 | 处置 |
| --- | --- | --- |
| 1 | 多 key 里只坏一个时，整组也会走指数退避，几轮后被拖到 15 分钟，健康 key 的数据跟着变旧；而 `lastOkMs` 每轮刷新会掩盖变慢 | **属实且严重，已修**。`refresh_plan.h` 新增 `groupDecision(total, failed)`（`showError` / `keepsSchedule`），`main.cpp` 新增 `sourcePartiallySucceeded`：调度完全复用成功路径（重置退避、按 50 秒排队），只把展示状态覆盖回异常。补两条原生测试并做变异验证。 |
| 2 | `age_seconds == 0` 无法区分「还没有成功记录」和「刚成功」 | **属实，已修**。改为 `-1` 表示没有记录，README 写明。 |
| 3 | `drawStatusLine` 的函数头注释与实现矛盾（说会一起丢掉 `*`，实际保留） | **属实，已修**。注释改为「先丢平台名，`*` 单独保留」。 |

审查者同时独立核实了上一轮「证书有效期」误报（`sdkconfig.h` 无 `CONFIG_MBEDTLS_HAVE_TIME_DATE`），并确认行基线 20/36/52 下视图 1/视图 2 三行之间留有 2～4px 间隙、未超出 64px。
未复核项：审查环境无法执行 `sync_ui_glyphs.py --check`（我这边已跑过并通过）；串口集成路径与 CLI payload 仍无自动化测试（既有测试条件所限，作为已知缺口保留）。

### 第 3 轮代码审查（Claude 系列 subagent：subagent_claude_code，最终工作区）

结论：**无严重问题**，四条改进建议：

| # | 意见 | 处置 |
| --- | --- | --- |
| 1 | `drawStatusLine` 的回退分支没有校验右侧状态词空间，注释里的「始终完整」没有代码保障 | **属实，已修**。排版抽成 `row_view.h::statusLineLayout`（纯算术），放不下时按平台名 → `*` → 过期时间的顺序让位，状态词永远完整；新增 `status_line_layout_never_overlaps` 五组用例，并用变异验证（去掉宽度校验后用例失败）。 |
| 2 | 最长状态词 < 128px、过期时间不被挤掉，都是没有测试守护的隐含前提 | **属实，已修**。新增 `status_words_stay_within_the_reserved_width`：遍历全部 `uiMessage()`，断言显示宽度 ≤ 84px，且在最宽状态词 + 平台名 + 一周的 `10080m` 下过期时间仍然显示。以后改文案会被拦住。 |
| 3 | `groupDecision(0, 0)` 把空 key 列表当成功，依赖 `configure` 侧校验兜底 | **已加注释**说明该前提，不改行为（`configure` 只接受 `keys.count > 0`）。 |
| 4 | README 的留白描述是定性的，没有像素级验证手段 | **保留**。留白由用户上机观感确认（并据此上移过 2px），文档措辞已改为定性描述，不写死像素值。 |

审查环境限制：该轮 Bash 沙箱只允许只读命令，审查者未能独立重跑 `pio test`/`--check`，改用源码级 grep 交叉核实字形扫描列表。

### 第 2 轮审查期间的用户要求与顺手修复（已包含在第 3 轮范围内）

- 行间距回到原来的 16px、整屏居中、上下各留半行左右空白（基线 18/39/60 → 22/38/54，随后又按用户观感上移 2px 到 20/36/52）。
- 视图 2 放不下平台名时仍保留 `*`（与视图 1 语义一致）。
- `erase` 之后立即重画，不用等最长 60 秒的空闲重绘。
- 这三处改动发生在第 2 轮启动之后，因此第 3 轮审查以最终状态为准。

### 第 1 轮代码审查（同模型 subagent：deepseek-official/deepseek-flash，HEAD 29ddedb + 工作区）

13 条意见、1 条误报，处置如下：

| # | 意见 | 处置 |
| --- | --- | --- |
| 1 | 冷启动时钟未就绪时，持久化的 429 冷却把 Claude 重试排到 7 天后 | **属实，已修**。`refresh_plan.h` 新增 `cooldownActive`/`cooldownRemainingSeconds`（时钟无效时不算冷却），`refreshClaudeUsage`/`scheduleSourceRetry` 改用；`recordClaudeCooldown` 在时钟无效时不写 Wall-clock 截止时间。补原生测试并做变异验证（改回旧逻辑用例即失败）。 |
| 2 | 时钟未就绪时 `drawWindowLine` 仍画出巨大的假 Δ（还超出 128px） | **属实，已修**。`now == 0` 时 Δ 与倒计时一起显示 `--`。 |
| 3 | 时钟未就绪时 HTTPS 会因证书有效期校验失败 | **误报，不采纳**。`tools/sdk/esp32c3/sdkconfig` 中 `# CONFIG_MBEDTLS_HAVE_TIME_DATE is not set`，ESP-IDF 不校验证书日期，无时钟也能握手。已在 README 写明。 |
| 4 | `view_phase` 与屏幕实际视图不一致；新字段没写文档 | **属实，已修**。无异常行时恒返回 1；README 补 `view_phase`/`rows_rotating`/顶层 `next_refresh_seconds`。 |
| 5 | 未配置余额时 `*_status.state` 显示「正在刷新」 | **属实，已修**。只有 `keys.count > 0` 才置 `kLoading`。 |
| 6 | 部分 key 失败也更新 `lastOkMs`，视图 2 显得「很新」 | **保留现行为**。产品口径是「该行上次成功获取」，部分 key 成功确有一次数据更新；失败 key 由视图 1 的 `*` 标记，且视图 2 平台名后也带 `*`。已在 README 与代码注释写明该口径。 |
| 7 | `last_refresh` 被余额刷新覆盖，语义模糊 | **保留字段、补契约**。README 明确它是「任一来源最近成功时间」，每行新鲜度看 `age_seconds`。 |
| 8 | 旧 CLI 字段行为与顶层 `next_refresh_seconds` 删除未记录 | **属实，已修**。顶层 `next_refresh_seconds` 恢复（取三个来源最早到期）；README 补旧 CLI 的兼容行为。 |
| 9 | 串行执行，README 的「互不拖慢」过度承诺 | **属实，已修文档**。改为「各自排队/退避，单线程串行执行，慢请求只推迟本次检查」。 |
| 10 | 刷新前那次 `drawScreen()` 是无变化的全屏 I²C 传输 | **属实，已修**。删除该次重画；进行态本来就不显示。 |
| 11 | 视图 2 每 4 秒创建 `String`，有堆碎片风险 | **属实，已修**。改为栈上 `char[12]`，逻辑抽到 `freshness_format.h::ageSince` 并加测试。 |
| 12 | `sourceFailed` 依赖全局 `uiState`/`lastErrorDetail` | **属实，已修**。改为显式传 state/detail。 |
| 13 | 测试覆盖不足（恒真断言、职责命名、缺冷却/`--` 用例） | **部分属实，已修**。删掉恒真断言改成相位交替校验；新增冷却与 `ageSince` 用例并做变异验证；`test_quota_behavior` 改名 `test_delta_format`。CLI payload 断言需要真实串口，作为已知缺口保留。 |
| 14 | 文档与实现不一致（NTP 周期、退避措辞、字段说明） | **属实，已修**。NTP 注释/README 统一为「每小时」；退避措辞改为「内部 30 秒起翻倍、实际不短于间隔」；各字段说明补齐。 |
