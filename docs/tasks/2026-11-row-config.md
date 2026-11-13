# 任务记录：可配置显示行与 ChatGPT 7d

## 当前阶段与下一步

- 阶段 4（交付）已完成：两笔功能提交 + 一笔文档修复已推送到 `origin/main`
  （`9a7ca3c` 2 秒轮换、`1c7f280` 可配置行与 ChatGPT、`2146075` 修复被截断的双视图记录），
  另有一笔文档状态更新。仓库没有 CI，不涉及发布流程。
- 三轮审查完成（DeepSeek ×2、Codex ×2），最终状态无必现 🔴；已接受的 🟡 见「已知遗留」。
- 交付前额外补修了 Claude 时钟未同步时丢凭据的问题（见「交付前补修」）。

## 目标（已定稿）

1. 屏幕行可配置：来源、顺序、窗口、每行刷新间隔；默认
   `chatgpt:7d,deepseek,openrouter`，最多 3 行，顺序即行序。
2. 第一行换成 ChatGPT 7 天窗口，代替 Claude；Claude 默认不抓取、不上屏，
   代码与凭据保留。
3. 保留 DeepSeek / OpenRouter 的多 key 余额：`+` 拼接、2 个及以上一位小数、
   逐 key 失败标 `*`、部分失败仍按正常节奏抓取。
4. 保留每行双视图轮换（每视图 2 秒）、1px loading 线。
5. 刷新间隔进入同一份配置，按行设置（用户 2026-11-13 决定）。

## 已确认的产品规则

- 配置语法：`source[:window][@interval]`，逗号分隔，最多 3 项；忽略两侧空格。
  - 来源：`chatgpt` / `claude` / `deepseek` / `openrouter`；
  - 窗口：`chatgpt`、`claude` 支持 `:7d`（默认）/ `:5h`，余额来源写窗口报错；
  - 间隔：`@30s` / `@5m` / `@1h`，范围 10 秒～24 小时；省略时窗口类默认 10 分钟、
    余额类默认 50 秒；同一来源占多行时抓取节奏取最短的间隔。
- 未列出的来源：不排队、不抓取、不上屏，凭据保留；移出配置时清掉它的状态
  （`status` 里显示未配置，不保留旧错误）。
- 布局：3 行基线 20/36/52，2 行 28/44，1 行 36；行距 16px；loading 线在基线 +2。
- 非法配置（空项、超过 3 行、未知来源、余额写窗口、非法间隔）由 CLI 与固件
  双重拒绝，设备保留原配置。
- `login --provider chatgpt`（兼容别名 `codex`）；`login` 默认仍是 `claude`。

## 实现

- 新增 `firmware/src/row_config.h`：行语法解析/回显、每行间隔、居中布局与
  「线不压字、不出屏、不同页」的不变量校验。
- 新增 `firmware/src/codex_protocol.h`：`rate_limit.primary_window` /
  `secondary_window` 解析，按 `limit_window_seconds` 归到 5h / 7d；非法响应保留旧值。
- `firmware/src/main.cpp`：
  - 新增 ChatGPT 源（OAuth 续期 `auth.openai.com/oauth/token` +
    `chatgpt.com/backend-api/wham/usage`，Bearer + `Chatgpt-Account-Id`）；
  - 行配置存 NVS `rows`，`configure` 接受 `rows` 并校验；`status` 回显 `rows`、
    新增 `chatgpt_status` / `chatgpt_five_hour` / `chatgpt_seven_day` /
    `chatgpt_configured`；
  - 调度、绘制、loading 线、`syncOverallState`、顶层 `next_refresh_seconds`
    全部按当前行配置动态计算；行配置读写走显示互斥锁；
  - 每行的抓取间隔参与成功调度、失败退避与视图 2 的过期时间阈值。
- `firmware/src/refresh_plan.h`：`retryDelay` 改为按行间隔；默认常量改名
  `kWindowRefreshSeconds` / `kBalanceRefreshSeconds`。
- CLI：`update --rows`（含 `@interval` 校验）、`login --provider chatgpt`、
  帮助文案；`parseRows` / `parseRefreshInterval` 有单测。
- 文档：README / firmware README 同步默认屏、配置语法、接口与字段。

## 上机验证（2026-11-13，设备 192.168.50.107，`/dev/ttyACM0`）

- 烧录成功（Hash verified）；默认 `rows=chatgpt:7d,deepseek,openrouter`。
- ChatGPT 7d 真实取到数据：`chatgpt_seven_day.present=true`、`used_percent=5`、
  `reset_at` 正常；`chatgpt_five_hour.present=false`（该账号无 5h 窗口）。
- 两行配置 `chatgpt:7d,deepseek`：回显正确；OpenRouter `next_refresh_seconds=0`
  且不再抓取；改回默认后三个来源立即恢复。
- 非法配置 `deepseek:7d` 被 CLI 拒绝；`chatgpt:5h,chatgpt:7d,deepseek` 接受。
- 每行间隔 `chatgpt:7d@15m,deepseek@20s,openrouter`：ChatGPT 从 894→867 秒递减
  （15 分钟）、DeepSeek 在 20 秒后重新抓取（next 15→2→9，age 归零）、
  OpenRouter 保持 50 秒节奏。
- 移出 claude 前后：包含时 `claude_status=登录失效`，移出后变为未配置。
- 动态行下的轮换：临时加入 `claude:7d`（凭据失效、无数据）后
  `rows_rotating=true`，`view_phase` 间隔 2003/2000/2000/2001/2003 ms；
  该行无数据所以常驻状态词、不闪（既有规则）。
- 验证后已把设备恢复为默认 `rows`。

## 未验证与已知限制

- 1 行 / 2 行的实际观感、5h 有数据时的显示、错误行有数据时的视觉轮换：
  需要用户目视确认（本机只能读状态，不能看屏幕）。
- ChatGPT 与 Claude 都采用「续期前落盘 pending、成功后单值原子写入」的保护；
  如果服务端在**请求失败**时也轮换 refresh token，仍需要重新登录（上游行为待核实）。
- 串口 `usage_provider` 仍返回 `claude`（保留兼容），权威字段是 `rows`。
- 设计草案：`docs/prototypes/2026-11-row-config.html`（已按定稿更新）。
- 视图 2 的过期时间：不到 1 分钟的间隔用秒（`<50s` / `55s`）；能被整分钟 /
  整小时 / 整天整除的用 `m` / `h` / `d`（`<10m`、`12m`、`<1h`、`2h`、`<1d`）；
  不能整除的（如 `@90s`）退回秒，避免不成立的阈值。这是用户 2026-11-13 确认的
  口径（原先 50 秒的行会出现 `<1m` / `0m`）。
- `rows` 回显总是带该行配置的 `@间隔`（用户 2026-11-13 选择显式），例如
  `chatgpt:7d@10m,deepseek@50s,openrouter@50s`；输入里省略 `@` 时回显会补上默认值。
  同一来源占多行时抓取取最短间隔，回显仍保留各行自己的配置值。
- 抓取仍是串行执行；慢请求会推迟同一轮里其他来源的本次检查（既有行为）。

## 验收清单（当前状态）

| # | 预期来源 | 条件 | 可观察结果 | 状态 |
| --- | --- | --- | --- | --- |
| A1 | 本次需求 | 默认配置 | 第一行 ChatGPT 7d，二、三行余额 | 通过（状态数据） |
| A2 | 本次需求 | `--rows "chatgpt:7d,deepseek"` | 两行居中、OpenRouter 停止抓取 | 通过（抓取侧） |
| A3 | 本次需求 | 不列 claude | 不发 Claude 请求、状态为未配置 | 通过 |
| B1 | 保留行为 | 多 key 余额 | `+` 拼接、逐 key `*`、健康 key 按间隔刷新 | 原生测试 + 既有上机 |
| B2 | 保留行为 | 出错行 | 每 2 秒轮换、各行同相位 | 原生测试 + 相位采样 |
| B3 | 保留行为 | 正在抓取的行 | 1px 线只在对应行、独立页；抓取结束后不残留高光 | 原生测试 + 上机观察待补 |
| C1 | 本次需求 | 非法 `rows` | CLI 与固件都拒绝，保留原配置 | 通过（CLI） |
| D1 | 本次需求 | `@interval` | 按行生效、同源取最短 | 通过（上机） |
| E1 | 本次需求 | 1/2/3 行观感 | 居中、不压字 | 待用户目视 |

## 追加决定（2026-11-13）

- 用户的设备配置为 `chatgpt:7d@3m,deepseek@50s,openrouter@50s`；
  已通过 `update --rows` 写入并实测 ChatGPT 3 分钟一抓。
- 固件默认值不变（窗口 10 分钟、余额 50 秒），3 分钟只作为该设备的 NVS 配置；
  `erase` 后会回到默认。
- `rows` 回显改为总是显式带 `@间隔`；过期时间按间隔选单位（<60s 用秒，整分/整时/整天用 m/h/d，不能整除则用秒）。

## 第 1 轮代码审查（DeepSeek 系列 subagent，只读）

1 条 🔴、10 条 🟡。处置如下：

| # | 意见 | 处置 |
| --- | --- | --- |
| 🔴1 | ChatGPT 续期先发请求后落盘、凭据分四个 NVS key 写，断电/写失败会留下不同代 token；401/403 不停止续期；未校验 access_token 非空 | **属实，已修**。ChatGPT 凭据改为单个 NVS JSON（`chatgpt_auth`，含 access/refresh/account/expires/pending/needs_login）；续期前先落 pending，响应丢失保留 pending，仅 429/5xx 清除；usage 401/403 置 needs-login 并停止自动续期；启动时把旧的四 key 迁移到单值；校验 access_token 非空。`status` 新增 `chatgpt_needs_login` / `chatgpt_pending`。 |
| 2 | `configure` 改 rows 后不重画，屏幕要等抓取或 60 秒空闲重绘 | **属实，已修**。应用行配置后立即 `drawScreen()`。 |
| 3 | loading 线在「多行切少行」时只做增量刷新，留下残余高光（既有缺陷） | **属实，已修**。`fullFlush` 改为只要 `mask != paintedMask`（含移除 bit）就整段补画。 |
| 4 | ChatGPT 重新登录/改凭据不清旧用量，可能显示上一账号数据 | **属实，已修**。`login` 与 `configure` 的 auth 分支清空 `chatgptFive/Seven` 和 `chatgptSource`。 |
| 5 | 时钟未同步时 `expiresAt=0` 会让每次抓取都续期并写 NVS | **属实，已修**。用单调时钟把续期限流到至少 5 分钟一次（`codexTokenRefreshMs`）。 |
| 6 | `saveSettings` 失败不回滚，RAM/NVS/rowConfigs 可能不一致 | **属实，已修**。`configure` 与 `login` 保存失败时恢复上一个 `Settings`。 |
| 7 | CLI 在解析串口后才校验 rows，测试名与实现不符 | **属实，已修**。所有纯参数校验（rows/时区/key）移到 `resolvePort` 之前，测试现在真实成立。 |
| 8 | 1h/24h 间隔显示 `<1440m` / `<60m`；10–59 秒的 `0m` | **属实，已修**。不到 60 秒用秒，≥1 小时用 `h`，≥1 天用 `d`。 |
| 9 | 同源多行时视图 2 用本行间隔、抓取用源最短间隔，口径矛盾 | **属实，已修**。视图 2 的新鲜度阈值也取该来源的有效间隔（最短）。 |
| 10 | 测试缺口：近恒真断言、`formatRows` 失败留半截输出、缺 10s/24h 与更多 codex 边界、`rowCount` 未 clamp、`used_percent>100` 被整份拒绝、429 无 Retry-After 冷却 | **大部分已修**。删掉恒真断言；`formatRows` 失败清空输出；补 10s/24h、窗口顺序、超额截断、未知长度全无效用例；`rowCount` clamp；ChatGPT 的 `used_percent>100` 截断到 100。**未做**：ChatGPT 429 的 `Retry-After` 冷却（沿用通用退避，记为已知限制）。 |
| 11 | README/固件 README/任务记录多处过时（固定三行、第一行 Claude、5h 不绘制、pending 只写 Claude、状态字段措辞等） | **属实，已修**。逐处改为按 rows 配置、窗口行通称、两套 OAuth 同等保护、未列出来源状态清为「未配置」等；任务记录补上草案链接与 B3 措辞。 |

审查者限制：只读，未跑 pio、未上机、未独立复现上机数据；429 冷却与上游是否轮换 refresh token 标为待核实。

## 第 2 轮代码审查（Codex / GPT 系列 subagent，只读）

结论：4 条 🔴、5 条 🟡，以及一份 DeepSeek 系列的补充复核。处置：

| # | 意见 | 处置 |
| --- | --- | --- |
| 🔴1 | `login` 撤消/重发可能把已被轮换的旧 refresh token 写回并清掉 pending（Claude 有持久化 provisionId 保护，ChatGPT 没有） | **属实，已修**。`chatgpt_auth` 里持久化最近一次 login 的 `request_id`；同一 request_id 的重发只确认、不重装凭据；`login` 改为只写凭据单值，不再连带写其他配置。 |
| 🔴2 | `saveCodexAuth` 不检查 cJSON 字段构造是否成功，可能写出缺字段记录；读取也不校验空 token | **属实，已修**。每个 `cJSON_Add*` 都检查非空，任一失败不落盘；读取要求 access/refresh 非空。 |
| 🔴3 | `configure` 的 auth 分支不清 pending/needs-login，新凭据会被旧失败状态挡住 | **属实，已修**。安装新凭据时清两个标志（与 login 一致）。 |
| 🔴4 | `saveSettings` 多 key 写入失败只回滚 RAM，NVS 可能部分生效 | **部分接受**。`login`/续期现在只提交凭据单值；`configure` 仍是多字段写入，属于既有设计，NVS 事务化未做，已记入限制。 |
| 🟡1 | 换账号后清数据与重绘顺序不对，旧账号画面可能残留 | **属实，已修**。`configure` 先应用 rows/auth/keys，再统一 `drawScreen()`；`login`、`claude_login` 清数据后也立即重绘。 |
| 🟡2 | 续期限流只覆盖成功路径，429/5xx/401 会绕过 | **属实，已修**。记录「尝试」时间（不是成功时间），主动续期至少 60 分钟一次；401 触发的续期至少间隔 60 秒，避免连环轮换。 |
| 🟡3 | 非整分钟/整小时间隔（如 `@90s`、`@90m`）会得到不成立的阈值（`<1m`） | **属实，已修**。按「能整除间隔的最大单位」选 m/h/d，否则退回秒；补了 90s / 90m 用例。 |
| 🟡4 | 关键状态转换（登录重放、pending、写失败、401）没有自动化测试 | **属实，未做**。原生测试不编译 main.cpp；要覆盖需要把存储/HTTP 抽成可替换接口，记为测试缺口，不在本次范围。 |
| 🟡5 | 降级再用旧固件登录后，再升级会优先旧的单值记录 | **属实，已修**。读取时只要旧的分散 key 存在就以它们为准并重新迁移，避免拿单值里的旧 token。 |
| 🟡6 | 文档冲突：429/Retry-After 写成通用、任务记录残留「ChatGPT 无 pending 保护」、`chatgpt_pending` 未写、Claude-only 登录提示缺失 | **属实，已修**。README 把 429/冷却明确为 Claude 行为、补 `chatgpt_pending`、改「设备断电/重启」措辞、步骤 4 补 rows 说明；固件 README 改「窗口行」；任务记录删掉过时描述。 |
| 🟡7 | 小项：formatRows 失败输出未断言、fullFlush 无单测、codex 注释 0..100 过时、login 的 provider 校验仍在串口之后 | **已修**：补失败清空断言、更新注释、provider 校验前移。fullFlush 决策仍在 main.cpp，无原生测试，保留「上机待补」；NVS 非事务已记入限制。 |

审查者限制：只读，未跑 pio/CLI 测试、未上机；上游 token 轮换、断电恢复与像素级显示待核实。

## 审查后状态

- 第 3 轮（最终状态）审查：完成，无必现 🔴（DeepSeek 与 Codex 各一轮）。

## 第 3 轮代码审查（最终状态）：Codex（GPT 系列）

结论：2 条 🔴、3 条 🟡。处置：

| # | 意见 | 处置 |
| --- | --- | --- |
| 🔴1 | 迁移无法区分「旧凭据更新」与「迁移残留」：任意一个旧 key 存在就遮住完整单值；删除旧 key 的结果未检查；降级后只改 Wi-Fi 也会写 `expires=0` 遮住凭据 | **属实，已修**。只有 access/refresh/account **三个都非空**的旧记录才覆盖单值；残缺残留（含只有 `expires`）不再遮挡，并在启动时由 `saveCodexAuth()` 清理；迁移后再次保存会重写单值。删除失败不再影响读取（残缺不参与判断）。 |
| 🔴2 | `configure` 安装凭据没有持久化去重，重发可装回已轮换的 token | **属实，已修**。凭据安装请求记录 `request_id` 到 `codexProvisionId`；`configure` 与 `login` 共用这一去重标识，重发时不重新安装凭据（其它字段照常处理）。 |
| 🟡1 | 429/5xx 清 pending 时忽略保存失败，RAM/NVS 分叉 | **属实，已修**。先写 NVS，成功才提交 RAM；失败保持 pending（保守停止）并报告存储错误。 |
| 🟡2 | 排版测试仍用 `<1m` 的 18px 宽度，真实 `<50s` 是 24px，首例断言不成立 | **属实，已修**。用例改为 24px，并区分「平台名放得下」与「必须让位」两种情况。 |
| 🟡3 | 文档口径：同源多行新鲜度、手动刷新冷却适用范围、限流 5 分钟 vs 60 分钟、退避上限与间隔的关系、`rows` 回显措辞 | **属实，已修**。README 改为按来源最短间隔、冷却注明 Claude、主动续期限流 60 分钟、退避上限说明不短于间隔、回显改为「该行配置的间隔」；`row_config.h` 注释同步。 |

审查者限制：只读；未跑 pio/CLI 测试、未上机、未做 NVS 故障注入；状态机测试缺口与 `configure` 多字段非事务仍保留。

## 冻结状态

- 最终固件已烧录：默认设备配置 `chatgpt:7d@3m,deepseek@50s,openrouter@50s`，
  ChatGPT 3 分钟抓取、真实 7d 数据、`needs_login=false`。
- 本轮之后不再改代码，等待最后一轮审查核对。

## 冻结前的最后一轮修补（2026-11-13 20:20）

- `claude_login`：重置 `claudeSource`、清 `claudeBlockedUntil`/NVS `claude_wait`，再重绘；
  换账号不会立刻显示旧错误或旧 age，也不会被旧 429 冷却拖住。
- `chatgpt_auth` 改用 NVS blob（string 单值上限约 4000 B，长 token 会写不进去），
  读取时兼容早期构建写下的 string，迁移后自动变成 blob。
- 更新迁移注释（完整旧记录才覆盖单值、残缺残留随迁移清掉）。
- 文档小项：README「另外两行」「三行共用」改为「其余行」「各行共用」。

**冻结点**：2026-11-13 20:20 +08:00，工作区未提交；此时间点之后不再改代码。
最终回归：原生 66/66、CLI 26/26、构建成功、字形 105 个、`git diff --check` 通过；
设备已烧录该版本并验证（`chatgpt:7d@3m,deepseek@50s,openrouter@50s`，7d 数据正常、
`needs_login=false`、`pending=false`）。

已知遗留（不在本次范围）：
- 凭据状态机（legacyComplete/configure 去重/pending 写失败）没有原生测试覆盖。
- `configure` 多字段 NVS 写入仍非事务。
- Claude 迁移残留的「完整旧 key 组」仍可能覆盖更新的单值（罕见路径，失败后会走一次
  400→pending→重新登录）。

### 交付前补修（2026-11-13 20:5x）：Claude 时钟未同步丢凭据

- 审查确认这是 HEAD 既有缺陷：`nowEpoch==0` 时续期写入 `epoch 0 + expires_in`，重启后
  `loadClaudeAuth` 因 `expires_at < 1.7e9` 丢弃整份凭据。
- 已修：新增 `claude::expiryFor(nowEpoch, expiresIn)`（时钟无效返回 0=未知），Claude 与
  ChatGPT 续期共用；`loadClaudeAuth` 接受 0；主动续期在时钟无效时用单调时钟限流
  （60 分钟）；401 触发的续期 60 秒内不重复；401/403 的 `pending` 写盘失败会报错并停止；
  `claude_login` 重置续期时间戳。
- 新增 `test_claude_protocol::expiry_is_unknown_without_a_clock`（67 个原生用例）。
- 已知限制：`expires_at == 0` 且时钟一直无效时，每次 401 都会经过 60 秒限流后尝试一次
  续期（不会打满端点）。
