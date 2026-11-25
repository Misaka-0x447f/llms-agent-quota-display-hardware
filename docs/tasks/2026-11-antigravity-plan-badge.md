# 任务记录：Antigravity 额度读取 + plan 徽标

## 当前阶段与下一步（2026-11-25 23:5x）

- 真机验证完成：4 行、徽标、Antigravity 7d、档位/余额都读到；`overage=false`。
- 代码审查 5 轮完成，第 4、5 轮无合并阻塞；审查后增量（4 行、UA、credits 解析、
  撤回低余额切换）已由真机 + 94 个原生用例 + 33 个 CLI 用例覆盖。
- 等用户确认交付（提交 + 推送 main；仓库无 CI）。

## 原始阶段记录

- 阶段 1、2 完成；用户 2026-11-25 20:19 确认「进入实施」。
- 阶段 3（实施、验证、审查）进行中：实现与本地验证完成，等待代码审查轮次。
- 目标体验草案：`docs/prototypes/2026-11-antigravity-plan-badge.html`（128×64 按 3 倍绘制）。

## 需求（用户 2026-11-25）

1. 追加 Antigravity 额度读取。
2. 有 plan 类型监控时，在 plan 额度重置时间前面加一个反色小 badge，只容纳一个字母：
   chatgpt `[O]`、gemini `[G]`、claude `[A]`。
3. Antigravity 只取 **7d** 额度。
4. 用户会给 Google 充钱；额度耗尽转消耗充值额度时布局特殊：`[G] [5d] 0%    xxx.xx`；
   credits ÷ 100 = 美元，不带 `$`。

## 已确认的决定

| 决定 | 结论 | 依据 |
| --- | --- | --- |
| 「antigravity 额度」指什么 | Antigravity 里 **Gemini 模型组自己的额度** | 用户答复 |
| 徽标出现条件 | **窗口行都带**（chatgpt / claude / antigravity），余额行不带 | 用户答复（覆盖了更早的「取到 plan 才显示」） |
| 徽标形态 | 反色实心块 + 镂空字母，9×11px，位于重置倒计时左侧，占 13px（9 块 + 4 间隔） | 用户「反色显示的小 badge」+ 尺寸按 7×14 字体推导 |
| 字母口径 | 按厂商首字母：OpenAI→O、Google→G、Anthropic→A | 用户映射表 |
| 切换 `$` 的条件 | **只在剩余额度 = 0% 时**切换（用户 2026-11-25 撤回「低于 minimum 也切」） | 用户答复 |
| 0% 但无 credits 数据 | 右侧显示 `--`（不编造 0.00） | 用户答复「0.00 或 --」，取保守一侧 |
| credits 单位 | **÷ 100**，两位小数，不带 `$` | 用户两次确认 |
| credits 读取方式 | `loadCodeAssist` → `paid_tier.available_credits[]`（`creditType=GOOGLE_ONE_AI`），**不发 probe、不消耗额度** | 本机 `agy` 二进制 proto + sub2api 实现交叉确认；用户确认 |
| 设备凭据入口 | `login --provider antigravity`，在电脑浏览器授权后写入设备 | 用户答复（**设备码改回环，见下**） |

## 已核实的事实

### Antigravity 接口（来源：本机 `~/.local/bin/agy` 二进制内的 proto descriptor + 开源实现交叉验证）

- 配额：`POST https://{daily-,}cloudcode-pa.googleapis.com/v1internal:retrieveUserQuotaSummary`，
  body `{}`（可选 `project`）。响应 `{"groups":[{"buckets":[{"bucketId","remainingFraction","resetTime","window"}],"displayName"}]}`，
  可能外包一层 `"response"`。
  - `bucketId`：`gemini-weekly` / `gemini-5h` / `3p-weekly` / `3p-5h`（3p = Claude/GPT）。
  - `remainingFraction` 是**剩余**比例（0..1），`resetTime` 是 RFC3339 `Z`。
  - 只有 `resetTime` 没有 fraction 的桶 = 已耗尽。
- 计划 + 充值余额：`POST .../v1internal:loadCodeAssist`。
  - `paid_tier.name` 优先于 `current_tier.name`（免费账号也报 `free-tier`）。
  - **`paid_tier.available_credits[]` 就带 AI Credits 余额**：
    `Credits{ credit_type(enum, GOOGLE_ONE_AI=1), credit_amount(int64), minimum_credit_amount_for_usage(int64) }`。
    所以读余额是**只读 POST**，不需要 OmniRoute 那种消耗 1 credit 的生成探针。
- 主机选择存在**互相矛盾的公开证据**：ai-usagebar 说 daily 的 summary 缺 Gemini 组、所以 quota 先走 prod；
  sub2api PR #8583 说 prod 会返回过期窗口、应该像生成一样走 daily；agy2dsh 的实测表里 daily 缺 Gemini 组。
  → 实现按「两个主机都试，取带 `gemini-weekly` 的那份；都带则优先 daily」处理，并把这个不确定性记入限制。
- 授权：`https://accounts.google.com/o/oauth2/auth`（回环重定向）+ `https://oauth2.googleapis.com/token` 续期。
  Antigravity 的公开 installed-app 客户端（从 agy 二进制提取）在
  `~/.gemini/antigravity-cli/*` 里没有本地凭据可用（本机未登录）。该 client 对
  （公开客户端，但仍被 GitHub push protection 当 secret）**不进仓库**，见「交付阻断」一节。

### 设备码授权：**技术上不可行**（已实测，2026-11-25 19:40）

- `POST https://oauth2.googleapis.com/device/code` 对 agy 里三个 client id 全部返回
  `{"error":"invalid_client","error_description":"Invalid client type."}`（其中一个明确说
  只有 “TVs and Limited Input devices” 类型的客户端能用设备码）。
  即用户先前选的「Google OAuth 设备码」无法用 Antigravity 自己的客户端实现。
- 同一客户端 + `http://localhost:54545/callback` 的授权请求返回 **302 到 Google 登录页**
  （不是 `redirect_uri_mismatch`），说明回环重定向被接受。
- **结论**：改用与现有 Claude / ChatGPT `login` 完全一致的回环流程（浏览器授权 → 粘贴回调 URL 兜底），
  用户的原始意图（电脑授权后把 refresh token 写进设备）不变，只改授权机制。

## 实现（已完成）

### 固件

- `row_config.h`：新增 `RowSource::kAntigravity`；`supportsFiveHour()` 让 `:5h` 只对
  chatgpt / claude 合法；新增 `rowBadgeLetter()`（O / G / A，余额行返回 nullptr）。
- `antigravity_protocol.h`（纯函数，原生可测）：`parseQuotaSummary`（`gemini-weekly` →
  7d 窗口，`remainingFraction` 是剩余比例，只有 resetTime 的桶按耗尽处理）、
  `parsePlan`（paidTier → currentTier）、`parseCredits`（`GOOGLE_ONE_AI` 的
  `creditAmount` / `minimumCreditAmountForUsage`，÷100）、`overageMode`。
  小驼峰与 snake_case、`{"response":…}` 包装都接受。
- `window_render.h`（新）：把窗口行绘制整体抽出来，用 `u8g2_t*` + C API，
  固件与电脑端渲染脚本共用同一份代码。含反色徽标（9×11 白块 + 镂空字母）、
  普通形态（Δ + 百分比贴右）、充值形态（百分比跟重置时间、金额贴右、不画 Δ）。
- `row_view.h`：新增 `windowRowLayout()` 与徽标/Δ 的像素常量；配速放不下时整组
  让位（改动前会压到百分比上）。
- `main.cpp`：Antigravity 凭据（单 NVS 值 `antigravity`，pending/needs_login/
  provisionId）、OAuth 续期、配额与档位/余额两次只读 POST（daily→prod 轮换）、
  调度与 loading 位、`antigravity_login` 串口命令、`status` 新字段。
- `platformio.ini`：native 测试加入 `test_antigravity_protocol`。

### CLI

- `google-oauth.ts`：Antigravity 的公开 installed-app 客户端 + 回环授权
  （`http://localhost:54545/callback`，`access_type=offline` + `prompt=consent`）。
- `cli.ts`：`login --provider antigravity`（先校验固件支持）、`--rows` 接受
  `antigravity` 且拒绝 `:5h`、帮助文案。

### 文档

- `README.md` / `firmware/README.md`：来源列表、徽标表、充值形态、接口与限制、
  电脑端渲染脚本的构建命令。

## 方案（已定稿，用户 2026-11-25 确认）

### 固件

- `RowSource` 增加 `kAntigravity`；`rows` 语法接受 `antigravity[:7d]`，`:5h` 报错（只取 7d）。
- 新协议头 `antigravity_protocol.h`（纯函数、可原生测试）：
  - `parseQuotaSummary(json, out)`：取 `gemini-weekly` 桶 → `RateWindow{usedPercent=(1-f)*100, resetAt, durationSeconds=7d}`；
    缺组/缺字段失败，调用方保留旧值。
  - `parsePlan(json, out)`：`paid_tier.name` → `current_tier.name`；`parseCredits(json)`：
    取 `credit_type == GOOGLE_ONE_AI` 的 `credit_amount` 与 `minimum_credit_amount_for_usage`（int64 字符串或数字）。
- 抓取：`refreshAntigravityUsage()`（配额 + 计划/余额两次 POST），凭证 `antigravity` 单 NVS 值
  （access/refresh/expires/pending/needs_login/provisionId），沿用两套 OAuth 的 pending + 原子写保护。
- 绘制：`drawWindowLine` 增加 badge 与 overage 两种形态：
  - 普通：`badge? [reset] Δ delta … percent`（percent 右对齐）。
  - overage（`remaining <= 0` 或 `credits < minimum`）：`badge [reset] percent … amount`（amount 右对齐，不画 Δ）。
  - badge 用 `setDrawColor(0)` 在实心块上镂空字母。
- `status` 新增：`antigravity_configured` / `antigravity_status` / `antigravity_seven_day` /
  `antigravity_plan` / `antigravity_credits`（`amount`、`minimum`、`present`）/ `antigravity_needs_login` / `antigravity_pending`。

### CLI

- `login --provider antigravity`：回环 OAuth（scope `cloud-platform`，可选 `userinfo.email`），
  写入 `antigravity`。
- `update --rows` 接受 `antigravity`，`:5h` 拒绝；帮助文案同步。
- `list` 回显新增字段（不含凭据）。

### 文档

- README：来源列表、默认配置说明、badge 规则、overage 布局、Antigravity 接口与限制；
  `firmware/README.md` 同步；任务记录本文件。

## 本地验证证据（2026-11-25 20:2x）

- 原生测试：`PLATFORMIO_CORE_DIR="$PWD/.pio-core" pio test -d firmware -e protocol_native`
  → **87/87 通过**（改动前 67 个；新增 `test_antigravity_protocol` 10 个、
  `test_row_config` 4 个、`test_row_view` 6 个）。
- 固件构建：`pio run -d firmware -e esp32c3_supermini` → SUCCESS。
- 字形：`python3 firmware/scripts/sync_ui_glyphs.py --check` → 108 个（新增 3 个，
  `ui_glyphs.h` 由构建脚本自动更新）。
- CLI：`tsc --noEmit` 通过；`node --import tsx --test tests/*.test.ts` → **31/31 通过**
  （新增 `tests/google-oauth.test.ts` 5 个）。
- 视觉：`firmware/scripts/render_rows_host.cpp` 用固件同一份 `window_render.h`
  渲染 8 个 128×64 帧（.shots/rows/*.pbm→png），逐张读过：徽标为 9×11 白块 +
  镂空字母，`[O] [3d] Δ-7h 40%`、`[G] [4d] 0% 12.34`、`[G] [--] … --` 排版正确，
  余额行不受影响。这是电脑端渲染，不是实物屏幕。
- `git diff --check` 通过。
- alint：`alint config inspect firmware/src/main.cpp` 与 `cli/src/google-oauth.ts`
  均显示 `rules:` 为空 → 仓库没有适用规则，按规则跳过 alint。

## 未验证（本次无法完成）

- 没有实物设备（`/dev/ttyACM*` 不存在），未烧录、未看屏幕；像素级观感、
  loading 线与徽标的相对位置仍待上机目视。
- 本机没有 Antigravity 登录凭据，真实的 `retrieveUserQuotaSummary` /
  `loadCodeAssist` 响应（尤其两个通道是否都带 `gemini-weekly`、是否有
  `availableCredits`）未实测；解析只对已知结构做了单元测试。
- Google 回环 OAuth 的完整往返（授权页 → 回调 → 换 token → 下发设备）未实跑，
  只用注入的 request 测了换 token 与参数。

## 代码审查

- 审查基准：`main` @ `8c034373a6dfbf7e40bd8989a8a21ba6668d4190`（= origin/main），
  本任务改动全部未提交（工作区 + 未跟踪文件）；排除 `.idea/git_toolbox_prj.xml`。
- alint：`alint config inspect firmware/src/main.cpp`、`alint config inspect
  cli/src/google-oauth.ts` 的 `rules:` 均为空 → 仓库无适用规则，按规则跳过
  `alint --dirty`（不是故障，后续轮沿用此结论）。
- 第 1 轮：DeepSeek 系列 subagent（与主 agent 同模型），只读审查。
  结论 1 🔴 + 9 🟡，处置见下。
- 第 2 轮：ChatGPT 系列 subagent（cliproxyapi / gpt-6.2-astra；该部署未声明
  reasoning effort，实际未传，记为与规则的偏差）。

## 第 1 轮代码审查（DeepSeek 系列，只读）：处置

| # | 意见 | 处置 |
| --- | --- | --- |
| 🔴1 | NVS 键名含 NUL 最多 16 字节，`antigravity_auth` 有 16 个字符 → `putBytes` 返回 0，凭据写不进、续期也发不出请求，整个功能不可用 | **属实，已修**。改用 `kAntigravityAuthKey = "antigravity"`（11 字符）+ `static_assert` 断言长度，4 处调用统一；本仓库之前从未写成功过，无需迁移。 |
| 🟡2 | 任一台主机 401/403 立即返回并标 needs_login，可能误伤「另一台 200」的情况 | **属实，已修**。记录 `authStatus`，只有**没有任何主机返回 200** 时才上报 401/403；两台都拒绝才算登录失效。 |
| 🟡3 | `fetchAntigravityPlanAndCredits` 拿到档位就返回，daily 缺 credits 时永远不问 prod | **属实，已修**。改为「拿到 credits 才停」，档位只附带记录；两台都没有 credits 才返回 false。 |
| 🟡4 | credits「拿不到时显示 `--`」在曾经成功过时不成立（保留旧值） | **属实，已修**。有 200 响应但没有 `GOOGLE_ONE_AI` 时清空（显示 `--`）；只有传输/HTTP 失败才保留旧值（与余额行口径一致）。 |
| 🟡5 | `>= 99.5` 与屏幕取整差半档：剩余 0.5% 显示 1% 却已切余额 | **属实，已修**。改为 `> 99.5`，并补 `overage_threshold_matches_the_rounded_percent` 用例（99.0 / 99.5 不切，99.6 / 100 切）。 |
| 🟡6 | 两处 User-Agent 不同，若上游校验具体 UA 会静默失败 | **部分接受，未改代码**。UA 取值来自两个已知可用的开源实现（配额 `antigravity`、档位 `agy`），本机无账号无法验证；记入「未验证」。不用同一个 UA 是因为配额端与档位端的既有实现本来就不同，擅自统一可能两边都 403。 |
| 🟡7 | 测试缺口：主机选择/401/凭据状态机无自动化；`overageMode` 边界未测；旧注释自相矛盾；徽标测试断言实现常量而非 13px；CLI login 无测试 | **大部分已修**。补 99.5 边界用例、把徽标断言改为 13px、配速断言收紧到「至少留 1px」、修正自相矛盾的注释、加 `login` 拒绝未知 provider 的 CLI 测试。**未做**：把 main.cpp 的主机选择/401/pending 抽成纯函数（需要改状态机结构，记为测试缺口）；`window_render` 的像素级断言（有 host 渲染 + 人工读图，未加自动断言）。 |
| 🟡8 | 文档与实现不一致（三套 OAuth、`antigravity_status`/`antigravity_account`、默认 rows 不含 Antigravity、固件 README 的凭据说明、`row_config.h`/`cli.ts` 注释、徽标白边描述、验收表状态） | **属实，已修**。逐处更新；徽标注释改为与实测一致的「左右约 2px、上下 0–1px」。 |
| 🟡9 | 死代码：`String formatDuration` 已无调用；`kFontHeight` 未使用 | **属实，已修**。两处删除。 |
| 🟡10 | `antigravityPlan` / `parsePlan` 属范围外附加物 | **保留**。`status.antigravity_plan` 对排查登录/档位问题有用，成本是一次已发出的请求里的解析；已在记录里标为可选。 |


## 第 2 轮代码审查（ChatGPT 系列 cliproxyapi/gpt-6.2-astra，只读）：处置

| # | 意见 | 处置 |
| --- | --- | --- |
| 🔴1 | **HTTPClient 会静默忽略 `addHeader("User-Agent", …)`**，实际发的是库默认 UA；上游按 UA 拒绝不认识的客户端 → 两个通道都会 403，且会被判成需要重新登录 | **属实，已修**。改用 `http.setUserAgent(userAgent)`。已核对本机 framework 源码（`HTTPClient.cpp:1040-1046` 过滤 UA、`:1206` 用 `_userAgent`）。同类问题：`getChatgptUsage()` 里的 `addHeader("User-Agent", "codex_cli_rs/…")` 也是无效写法（既有代码，ChatGPT 接口不校验 UA，本次不扩大范围，记入观察）。 |
| 🔴2 | 一台 403 + 另一台 503/超时，或 200 缺桶 + 403，仍会判 needs_login（混合结果没有被第一轮修复覆盖） | **属实，已修**。`fetchAntigravityQuota` 改为返回 `QuotaFetch{kOk, kAuthRejected, kBadData, kTransient}`：只有**所有尝试的主机都明确 401/403** 才算登录失效；200 但缺桶是数据错误；其余（含混合结果）是暂时失败、保留旧值并退避。 |
| 🔴3 | Google 授权码流程没有 PKCE，回环端口或粘贴回调被截获时可由第三方换取令牌 | **属实，已修**。加 S256 PKCE（每次登录新 verifier，授权带 challenge，换 token 带 verifier），并补两个测试（challenge 与 verifier 匹配；两次登录 verifier/state 不同）。该客户端在真实 Google 链路上的兼容性仍待实测。 |
| 🟡4 | credits 解析只有 true/false，空/截断的 200 响应会被当成「明确没有余额」而被清空 | **属实，已修**。改三态 `CreditsStatus{kAbsent, kFound, kInvalid}`；只有 `kAbsent` 清空，`kInvalid` 继续问下一台主机、两台都无效则保留旧值。 |
| 🟡5 | 宽松的「gemini* + weekly」匹配可能把分模型桶当成组额度，且 daily 命中后不再问 prod 的精确桶 | **属实，已修**。只认精确 `gemini-weekly`；删掉宽松分支，补「`gemini-4-argon-weekly` 不算组额度」用例。代价是上游若换 id 会显示占位符（比显示错额度好）。 |
| 🟡6 | 窗口有比例、无 resetTime、时钟未同步时显示 `[7d]` 而不是 `--` | **不采纳（既有行为）**。这是改动前 `drawWindowLine` 就有的回退：`resetAt == 0` → `[7d]/[5h]`，`resetAt != 0 && now == 0` → `--`；README 也这样写。本次保持旧行为，不顺手改。 |
| 🟡7 | 极宽金额时百分比与金额同时隐藏、测试枚举抓不到 | **部分接受，已修**。布局新增 `showReset`：金额连重置时间都放不下时，保留金额、让掉重置时间与百分比（金额是充值形态的信息主体），不再留白；补对应测试，枚举里正常量级仍断言三者可见。 |
| 🟡8 | 串口 `expires_at` 接受 1..UINT32_MAX，但读盘要求 0 或 ≥ `kMinimumValidEpoch`，「登录成功」后重启会丢凭据 | **属实，已修**。串口改用 `claude::integer(expires, kMinimumValidEpoch, UINT32_MAX)`，与读盘同一口径。 |
| 🟡9 | 文档：README 说「上游没有 5 小时桶」与实际不符；实现说明仍是旧 NVS 键名；验收表全「待实施」且 A3 把「缺余额」当成切换条件 | **属实，已修**。README 改为「本产品只读周桶、上游有 `gemini-5h` 但不读」并补「只认精确 bucketId」；实现说明用实际键名；验收表拆成 A3/A4/A5。 |
| 🟡10 | `render_rows_host.cpp` 输出目录不可写时仍报成功 | **属实，已修**。`dump` 返回结果，失败打印路径并以退出码 1 结束（已实测：`/nonexistent-dir` → 1）。 |

## 第 3 轮代码审查（ChatGPT 系列，只读）：处置

| # | 意见 | 处置 |
| --- | --- | --- |
| 🔴1 | credits：daily 合法但没带余额时 `kAbsent` 立即返回，prod 上真实存在的余额永远读不到 | **属实，已修**。改为 `sawAbsent` 记录后继续问下一台；任一主机 `kFound` 即采用；试完仍未找到且有 `kAbsent` 才清空。 |
| 🔴2 | 低余额切换时屏幕显示真实剩余（如 37%），与定稿的 `0%` 布局不一致 | **待用户确认（不改代码）**。定稿里的 `0%` 描述的是「额度耗尽」那一态；低余额但周额度仍有剩余时，显示真实百分比才不误导。当前行为是 `[G] [5d] 37%  4.99`，`status` 也保留真实剩余。交付汇报里作为 UX 取舍请用户拍板，不擅自把 37% 画成 0%。 |
| 🟡3 | 混合失败（403+503）不再锁死续期，但错误状态词仍可能是「登录失效」 | **属实，已修**。只有全部主机明确拒绝才返回 `kAuthRejected`；一台拒绝 + 一台不可用时报「Antigravity 接口暂时不可用」（`kHttpError`），不写成登录失效。 |
| 🟡4 | 连续无效的 credits 响应会无限期保留旧金额；`{"response":[]}` 被误判为「明确没有」 | **属实，已修**。连续 2 次无效响应就清空（单次坏响应仍保留旧值）；`parseCredits` 把非对象的 `response` 包装判为 `kInvalid`。 |
| 🟡5 | 极宽金额会压住徽标，超过 128px 时全部隐藏但测试只测到 100px | **属实，已修**。金额宽到压住徽标时徽标让位；超过整行时明确不画金额；补 119/126/128/129px 用例。 |
| 🟡6 | host 渲染器只处理了 `fopen` 失败，`fwrite`/`fclose` 的流错误仍会报成功 | **属实，已修**。检查 `fflush`/`ferror`/`fclose`，失败打印并以 1 退出。 |

## 追加需求（用户 2026-11-25 21:5x）：最多 4 行

- `kMaxRows` / CLI `MAX_ROWS` 从 3 改成 4；4 行配置合法，5 行仍然拒绝。
- 4 行时行距 16px → **14px**（基线 15/29/43/57），否则 64px 高度放不下第四行；
  1～3 行保持原基线。`rowLayoutFits` 的不变量（线不压字、不出屏、每线独占一页）
  对 4 行仍成立：线在 17/31/45/59，页 2/3/5/7。
- 代价：4 行时 loading 线离下一行文字只有 1px（3 行时是 2px），顶部墨迹从 y=4 开始。
- 测试：`four_rows_parse_and_format`、4 行布局基线/行距/首行不出屏、
  `layout_check_rejects_unsafe_rows` 仍用 3 行样例；CLI 侧断言 4 行合法、5 行拒绝。
- 文档：README / 固件 README 的「最多 3 行」与基线说明同步。

## 用户最终确认（2026-11-25 21:3x）

- 徽标改为 9×12（向上补 1px 白边）后**确认通过**。
- 低余额但周额度仍有剩余时的百分比：**保持真实剩余**，不改画成 0%。
  用户原话：「这里的百分比无论如何都不应该是假的」。`0%` 只对应额度真的耗尽。
  README 与验收表 A3 按此口径定稿。

## 审查后调整（用户 2026-11-25 21:16）

- 「badge 的背景反白往上补 1px」：方块从 9×11 改为 **9×12**（`baseline-11` 起、高 12），
  字母上下各留 1px 白边（改前顶部为 0px）。新增 `quota::kBadgeHeight` 并在
  `test_row_view` 里钉住 9×12 这两个用户可见尺寸；重新渲染 8 帧逐张核对，
  徽标区域实测为「白行 / 字母 10 行 / 白行」。README 与原型同步。
- 这次调整在 4 轮审查之后，属于 1px 渲染参数变化；已重跑 92 个原生用例、构建、
  字形校验和 8 帧渲染，CLI 未受影响。

## 第 4 轮代码审查（ChatGPT 系列，只读）：处置

第 4 轮结论：**无合并阻塞问题**，4 条改进建议。

| # | 意见 | 处置 |
| --- | --- | --- |
| 🟡1 | `"paidTier": []` / `availableCredits:[null]` 这类「字段在但类型错」被判成 `kAbsent`，会在第一次就清空余额，绕过坏响应保护 | **属实，已修**。`paidTier` 非对象非 null、列表元素非对象都返回 `kInvalid`；补三条用例（含 `{"response":[]}`）。 |
| 🟡2 | 「连续两次无效」的口径含糊：中间没有响应的轮次不计数，反之一直是 503 时旧金额永不过期 | **属实，已修**。去掉 streak，改为「最后成功时间 + 30 分钟保鲜期」（3 个默认窗口周期）：超过就清空，两种情形都覆盖。 |
| 🟡3 | 超宽金额会先把倒计时/徽标让掉、最后连金额也不画 → 整行变空；布局降级边界没有直接断言 | **属实，已修**。金额比整行还宽时改画 `--`（保住行身份）；补 65px（去空格）/72px（去掉百分比）两个边界的直接断言。 |
| 🟡4 | 输出目录路径被 `snprintf` 截断时仍会报成功 | **属实，已修**。新增 `framePath()` 检查返回值，超长直接报错并返回非零。 |

## 真机实测：Gemini 额度读通了（2026-11-25 22:0x–23:3x）

**根因是 User-Agent，不是客户端准入。** 同一个设备令牌、同一台主机，只换 UA：

| UA | 结果 |
| --- | --- |
| `antigravity` | 200，只有 `3p-weekly,3p-5h` |
| `antigravity/1.15.7` | 200，只有 3p |
| `antigravity/1.15.7` + `X-Goog-Api-Client` | 200，只有 3p |
| `google-api-nodejs-client/9.15.1` + Api-Client | 403 |
| **`antigravity/hub/2.23.0 windows/amd64`** | **200，`gemini-weekly`/`gemini-5h`/`3p-weekly`/`3p-5h` 全有** |

`loadCodeAssist` 同理：`agy` UA 回 `ineligibleTiers: UNSUPPORTED_CLIENT`，hub UA 回
`paidTier = Google AI Pro` + `availableCredits`。线索：
[sub2api #8578](https://github.com/Wei-Shaw/sub2api/pull/8578)（summary 接口对 UA 有门禁）、
[CLIProxyAPI 常量](https://pkg.go.dev/github.com/router-for-me/CLIProxyAPI/v6@v6.8.52/internal/auth/antigravity)（`X-Goog-Api-Client`）。

已把配额与档位都改成 hub UA。设备实测：`antigravity_seven_day.present=true`、
`used_percent ≈ 27%`（剩余 ≈73%，与 `agy` 报的 75% 同量级）、`reset_at` 正常；
`antigravity_plan = "Google AI Pro"`；
`antigravity_credits = {present: true, amount: 0, minimum: 0.5}`。

顺手修掉的两个真实问题：
- `availableCredits` 条目可能**只有 `minimumCreditAmountForUsage` 而没有
  `creditAmount`**（= 余额 0），之前被判成坏数据保留旧值；现在按 0 处理。
- 档位/余额原先只在配额成功后才抓；改成先抓，配额失败也能显示档位与余额。

## 用户撤回「低余额也切金额」（2026-11-25 23:3x）

- 低余额（credits < minimum）**不再**触发金额形态：否则没有 credits 的账号会永远
  看不到 Δ 配速。
- 现在只在周额度用尽（屏幕 0%）时切到 `[G] [Nd] 0%   xxx.xx`。
- `overageMode` 只按 `usedPercent > 99.5` 判断；单测与 README 已同步。

## （历史）最初误判：以为是 Google 关闭了第三方 Gemini 额度

## 第 5 轮代码审查（ChatGPT 系列，增量）：处置

结论：无合并阻塞。6 条改进的处置：

| # | 意见 | 处置 |
| --- | --- | --- |
| 🟡1 | 「缺 `creditAmount` 当 0」的认定过宽：只有 `creditType`、或 `creditAmount:null` 且无 minimum 也会被当成 0 并清掉旧值 | **属实，已修**。只有「amount 缺失 + minimum 有效」才按 0；两个字段都缺算 `kInvalid`（保留旧值）。补 4 个用例（minimum-only 正例、只有 creditType、显式 null、坏响应不改已有非零余额）。 |
| 🟡2 | 被动续期后只重试配额，档位/余额仍可能用旧令牌的结果 | **属实，已修**。续期成功后先重抓一次档位/余额（因此 project 也是新的），再重试配额。 |
| 🟡3 | `project` 手拼 JSON 未转义；缓存只在成功时覆盖 | **属实，已修**。改用 cJSON 构造 `{"project": …}`。缓存语义保持「最近一次成功响应里的项目」，登录/清空时重置，已在注释里写明。 |
| 🟡4 | 成功路径也整份复制响应；错误文案与括号里的 bucket 列表矛盾 | **属实，已修**。只在解析失败时保存响应做摘要；文案改为「配额响应里没有可用的 Gemini 每周额度」。 |
| 🟡5 | hub UA 版本写死，将来上游变卦会持续 403 并落 `needsLogin` | **接受，仅记录**。UA 取自已验证可用的客户端格式，不自动轮换到已知只回 3p 的 UA；风险写进「未核实 / 待上机」。 |
| 🟡6 | 渲染脚本手写 4 行基线、手动指定低余额模式，可能与产品行为脱节 | **属实，已修**。4 行基线改从 `rowLayout(4)` 取；低余额场景用 `overageMode()` 判定，若 73% 剩余被切成金额形态脚本会报错退出。过时注释同步。 |

## 交付阻断与处理（2026-11-26 00:0x）

`git push` 被 GitHub push protection 拒绝：提交里含 Antigravity 的
**Google OAuth Client ID / Client Secret**（`GH013`）。这两个值虽然是公开的
installed-app 客户端凭据，但 GitHub 的规则不区分「公开客户端」与「用户凭据」，
自动审查也拒绝带着它们外发。

处理：把两个值从仓库移出，改为运行时/构建时注入。

- `cli/antigravity-client.json`（gitignore）：本地客户端配置；
  `cli/antigravity-client.example.json` 入库，只放占位符。
- `cli/src/antigravity-client.ts`：优先读环境变量，其次读上面那个文件；缺失时在
  打开浏览器前给出可执行的报错。
- `firmware/scripts/gen_antigravity_client.py`：从同一来源生成
  `firmware/src/antigravity_client.h`（gitignore），由 PlatformIO pre-build 钩子调用；
  未配置时生成空串，设备显示「Antigravity 客户端未配置」，不发空 client_id 请求。
- 测试改用假客户端（`test-client.apps.googleusercontent.com`），不再断言真值。
- 两个提交重写（内容不变的部分重新提交），推送前用 `git grep` 确认工作区与
  提交里都没有这两个值的字面量。

## 验收清单（草案）

| # | 预期来源 | 条件 | 可观察结果 | 状态 |
| --- | --- | --- | --- | --- |
| A1 | 本次需求 | `rows` 含 `antigravity:7d` | 屏幕显示 `[G] [Nd] Δ… N%`；`status` 有 `antigravity_seven_day` | **通过（真机）** |
| A6 | 本次需求 | 最多 4 行 | `update --rows` 接受 4 项、拒绝 5 项；设备 4 行渲染正常 | **通过（真机）** |
| A2 | 本次需求 | Gemini 7d 用尽、credits ≥ minimum | 同一行变 `[G] [5d] 0%  xxx.xx`（÷100、无 $、无 Δ） | 待实施 |
| A3 | 本次需求 | 周额度未耗尽但 credits < minimum | **不切换**，仍显示 Δ 与真实百分比（用户 2026-11-25 撤回该规则） | **通过（真机，73% + 0 余额）** |
| A4 | 本次需求 | 周额度耗尽且**没有**余额信息 | 右侧 `--`，不显示 0.00 | 待实施（无耗尽账号） |
| A5 | 本次需求 | 周额度未耗尽且没有余额信息 | 保持 Δ 形态 | 待实施 |
| B1 | 本次需求 | chatgpt / claude 窗口行 | 分别显示 `[O]` / `[A]`，位置固定 x=0 | 待实施 |
| B2 | 保留行为 | 余额行、1/2/3 行居中、视图 2 轮换、1px loading 线 | 与现状一致；余额行无徽标 | 待实施 |
| C1 | 本次需求 | `antigravity:5h` | CLI 与固件都拒绝，保留原配置 | 待实施 |
| D1 | 本次需求 | `login --provider antigravity` | 浏览器授权 → 凭据写入 → 首次抓取成功 | 待实施 |

## 未核实 / 待上机

- 真实账号的 `retrieveUserQuotaSummary` 是否两个主机都返回 `gemini-weekly`（evidence 冲突）。
- `loadCodeAssist` 是否需要 `project` / 额外的 `metadata`；本机无 Antigravity 账号，无法实测。
- `credit_amount` 的单位（用户确认 ÷100；与 sub2api 的「原值」用法不同，上机后以真实数字复核）。
- 充值余额的「低于 minimum 就切换」在真实账号上的表现（minimum 是否存在、是否 0）。
- 屏幕实际观感（像素级）：徽标与 7×14 正文的贴合、`[G] [5d] 0%` 与金额间距。
- 设备已烧录版本与本任务无关；本次上机验证需要真实 Antigravity 登录。
