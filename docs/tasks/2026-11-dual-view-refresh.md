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

## 追加需求：1px loading 动画（用户新提出）

- 需求：哪一行正在抓取，就在该行下面画一条 1px 的 Material 风格 indeterminate
  进度条，只占 1px。
- 取舍（用户选择）：请求期间主循环是阻塞的，动画必须放在独立任务里才动得起来；
  用户接受新增一把显示互斥锁和并发逻辑。
- 实现：
  - `row_view.h::loadingBarSpan`（高光位置）与 `loadingBarFlush`（只算需要重发的
    tile 列，新旧位置不相交时拆两段）都是纯函数，配 4 条原生测试。
  - 线在行基线 **+2**（22/38/54），分属 2/4/6 三页；`static_assert` 用实测的
    「最大上伸 11 行、最大下伸 1 行」锁住「不压本行、不压下一行、不出屏、不同页」。
  - 字体上下界用主机端编译 u8g2 实际渲染量出：`[` 顶到基线 −11，7×14 的
    `D-4h`/`27%` 为 −10..−1，6×12 的 `deepseek`/`openrouter` 因 `p` 到 +1。
    所以 +2 紧贴本行、离下一行 ink 还有 2px；+3 会两边各差 1px，反而像属于下一行。
  - loading 动画的开关是独立的 `fetchingMask`（bit0/1/2），只在 `refreshSources`
    里置位、用 RAII 在函数返回时清零；`loadSettings`/`resetBalanceGroup` 不再写
    `kLoading`。这样「刷新循环从未启动」的配置（有 Claude 凭据但没配 Wi-Fi、
    或只配了余额 key）不会留下一条永久转动的线。
  - `drawScreen` 不再画 loading 线，线只由动画任务画/擦，避免「全屏画过但任务
    的 previousX 不知道」造成 boot 残影。
  - `loadingAnimationTask` 每 60 ms 一帧，锁内读一次 `fetchingMask`；每帧只
    `updateDisplayArea` 高光新旧位置覆盖的几列；`displayMutex` 串行化所有显示访问，
    互斥锁或任务创建失败时打印错误并退化为「无动画、照常绘图」。
  - 来源在发起请求前先置 `kLoading`（不算异常，不触发视图 2），抓完立刻覆盖。
- 实测（串口临时埋点，验证后已移除）：软件 I²C 约 1.4 KB/s（一整页 128 B ≈ 90 ms、
  整屏 ≈ 0.9 s）。按 tile 列刷新后的帧成本（含埋点自身约 4 ms/帧的串口开销）：

  请求阻塞期间动画帧连续推进（`REQ` → 收线帧之间没有断档），`fetchingMask`
  也从「联网阶段 7」逐个切到单来源位。

- 帧率优化（用户反馈「帧数很低」后）：原实现每帧把整段 32px 高光重发，改成
  「只刷变化的两端」（`loadingBarFlushDiff`）并把帧间隔 60 ms → 25 ms；
  整段重画只在起步、行数变化、整屏重画之后做一次（`loadingBarNeedsFullFlush`）。

  | 场景 | 优化前 | 优化后 |
  | --- | --- | --- |
  | 单来源 OpenRouter | 11.1 fps / 35 B | 21.5～26.5 fps / 15～16 B |
  | 单来源 Claude | — | 22.1 fps / 16 B |
  | 单来源 DeepSeek | 10.4 fps / 41 B | 17.4 fps / 23 B |

  三行同时抓取时按 3 倍字节估算约 16 fps（未单独采样：WiFi 已连接时批次会
  瞬间从「全部到期」切到单来源，三行同时在刷新的窗口太短）。
- 上机：烧录后 OpenRouter 首次抓取出现一次 `TLS PADLOCK - Input data should be
  aligned`，下一次 50 秒重试即恢复（`age_seconds` 归零），之后多轮抓取正常。
  该现象只在单次出现，未做「临时关掉动画任务」的对照实验，因此**因果未证实**：
  既可能是站点偶发 TLS 失败，也可能是动画任务分走 CPU 的副作用，待复现时再查。

### 追加需求的第 1 轮审查（同模型 subagent：deepseek-official/deepseek-flash）

| # | 意见 | 处置 |
| --- | --- | --- |
| 🔴1 | 「刷新循环从不启动」的配置（有 Claude 凭据但没配 Wi-Fi，或只配余额 key）会让 `loadSettings` 置的 `kLoading` 永久转动 | **属实，已修**。动画开关改成独立的 `fetchingMask`，只在 `refreshSources` 里置位、用 RAII 在函数返回时清零；`loadSettings`/`resetBalanceGroup` 不再写 `kLoading`。 |
| 🟡1 | `drawScreen` 也画线，但动画任务的 `previousX` 不知道，boot 慢发送可能留残影 | **属实，已修**。线只由动画任务画/擦；整屏重画不再画线，也就没有「全屏画过而任务不知道」的耦合。 |
| 🟡2 | `static_assert` 用了 ascent 10，但实际最大上伸是 11，断言锁不住「不压下一行」 | **属实，已修**。主机端渲染实测：`[` 顶到 −11、`p` 到 +1；改成 `kTextMaxTopRows=11`/`kTextMaxBottomRows=1`，线改到基线 +2（+3 会两边各差 1px，反而像属于下一行）。 |
| 🟡3 | 互斥锁/任务创建失败未处理，`xSemaphoreTake(nullptr)` 会 panic | **属实，已修**。两处创建失败都打印 `ERROR ...` 并降级为「无动画、照常绘图」；所有 take/give 都有 null 保护。 |
| 🟡4 | 动画任务在锁外读 `state`，同一帧读两次可能不一致 | **属实，已修**。改为锁内读一次 `fetchingMask`（单字节 volatile，写端只有 loopTask）。 |
| 🟡5 | 只刷差集可再省 3 倍 I²C | **不采纳（记录理由）**。去掉「整屏画线」后，并集刷新是自洽的：任何时刻只靠 `previousX` 就能同时擦旧画新。改成差集必须在每次整屏重画后额外强制刷全段，正好是 🟡1 那类耦合。当前单来源 35～41 B/帧、8.7～11 fps 已够用。 |
| 🟡6 | `anyRowLoading` 死代码、三行列表重复、魔法数 | **已修**。删除死函数；新增 `RowSlot kRows[]` 表 + `kDisplayWidth`/`kRowCount`，绘制与任务共用。 |
| 🟡7 | 回卷用例断言太弱、注释过期、tracker 无测试 | **部分已修**。注释改为 32px/2.4 秒；回卷用例改成断言具体 tile（15@1 与 0@1）并补「部分出屏/完全出屏」用例。tracker（`painted`/`previousX`）仍在任务里，未抽成可测结构，作为已知缺口保留。 |
| 🟡8 | 文档与实现/实测不一致 | **已修**。字体理由改成实测值；帧率表换成复测数据；README「单线程」改为「抓取串行、动画独立」；「屏幕不使用进度条」改为只指额度条；PADLOCK 结论改为「因果未证实」。 |

### 追加需求的上机观察（2026-11-11 23:5x）

- 动画固件跑通后（三行全绿、10.4/11.1 fps）出现过一段设备侧网络抖动：先后报
  `DNS失败：openrouter.ai` 与 `connection refused`（Claude/DeepSeek），期间串口
  `list` 有一次 20 秒无响应。主机侧同一时刻对三个接口均可达（HTTP 401），因此判断是
  设备侧 WiFi/网络瞬断，与本改动无关（同一动画固件在抖动前刚验证过三行全绿）。
- 由此暴露一个既有问题（非本次引入）：设备在网络故障时会在 `connectWiFi`（≤15 s）
  与各来源的 HTTP 超时里阻塞，单次可达数十秒，期间串口命令不被处理，`list` 会超时。
  记录为已知限制，本次不改。

### 追加需求的第 2 轮审查（Claude 系列 subagent：subagent_claude_code）

结论：**无严重问题**。处置：

| # | 意见 | 处置 |
| --- | --- | --- |
| 1 | 字体边界「实测」只给了结论，没有可复跑的产物，`p` 是否到 +1 无法独立核实 | **已补证据链**。新增 `firmware/scripts/font_ink_probe.c`（离线渲染实际字体、回读 ink 行范围，带运行方法），输出记录在此：`[6d]` → −11..+1、`deepseek`/`openrouter` → −7..+1、`12.34` → −10..0。故 `kTextMaxTopRows=11`、`kTextMaxBottomRows=1` 成立，线在 +2 不压字。 |
| 2 | `firmware/README` 写「整屏重画只在状态变化时发生」，但 `loop()` 每次到期刷新后都无条件 `drawScreen()` | **属实，已修文档**。改为「整屏重画沿用既有节奏（每次到期刷新后都做一次，不论数值是否变化），动画只在两次之间补实时反馈」。 |
| 3 | 完全空闲时动画任务仍每 60 ms 抢一次显示锁 | **属实，已修**。先无锁判断 `fetchingMask/paintedMask`，空闲时直接延时；锁内再读一次状态。 |
| 4 | 任务创建失败时创建出来的互斥锁不会被释放 | **属实，已修**。失败分支 `vSemaphoreDelete` 并置空，统一走「没有锁就不锁」的降级路径。 |
| 5 | 「联网阶段点亮所有 due 行」与 README「只有真正发起抓取的行才有线」不一致 | **属实，已修文档**。README 写明 WiFi 重连阶段（最长 15 s）本轮到期的行都会亮线，联网成功后切成只点亮正在请求的那一行。 |

### 用户上机反馈与追加修正（2026-11-12 00:0x）

- 「loading 跑到 deepseek 去了」：**不是缺陷**。当时 Claude 行处于 `TCP被拒绝` 的 10 分钟退避里，屏幕上那条线是 DeepSeek 自己每 50 秒的抓取，位置正确。
- 「claude 那行一闪一闪的」：**属实，是设计缺陷**。该行当时没有任何数据，视图 1 是空白，于是每 4 秒在「空白」与「状态词」之间切换，看起来像故障。
  - 修法：`showStatusView(hasError, hasData, phase)` —— 出错但还没有数据的行常驻视图 2，不再轮换；有数据后才恢复 4 秒轮换。补 `dataless_error_row_never_blinks` 用例。
  - 顺带：没有数据时视图 1 不再整行空白，Claude 行显示 `[--] ... --` 占位，与余额行的 `--` 一致。
- 「c 小写」：视图 2 的 Claude 行平台名改为 `claude`，与 `deepseek` / `openrouter` 一致。

### 追加需求：帧率优化（用户反馈后）

- 改动：`row_view.h` 新增 `loadingBarFlushDiff`（只算新旧高光的对称差，即两端）；
  `main.cpp` 增加 `loadingBarNeedsFullFlush`（整屏重画/起步/行数变化时整段补一次），
  帧间隔 60 → 25 ms。补 `loading_bar_diff_only_flushes_the_changed_edges` 与
  `loading_bar_full_flush_covers_both_positions` 两条用例。
- 正确性要点：增量刷新只发对称差覆盖的 tile，中间没变的像素保持不动；回卷或
  大跳变时退化成「擦旧 + 画新」两段，仍不会整页重发。
- 实测见上表；本次改动发生在第 2 轮审查之后，只有单元测试与上机核对，未再单独开审查轮。

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
