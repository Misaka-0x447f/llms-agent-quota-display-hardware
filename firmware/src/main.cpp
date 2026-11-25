#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <cJSON.h>
#include <esp_sntp.h>
#include <time.h>

#include "ui_glyphs.h"
#include "antigravity_protocol.h"
#include "claude_protocol.h"
#include "codex_protocol.h"
#include "row_config.h"
#include "freshness_format.h"
#include "refresh_plan.h"
#include "row_view.h"
#include "window_render.h"
#include "delta_format.h"
#include "balance_protocol.h"
#include "balance_format.h"
#include "key_list.h"


namespace pins {
constexpr uint8_t kI2cClock = 9;
constexpr uint8_t kI2cData = 8;
constexpr uint8_t kKeyStar = 3;
constexpr uint8_t kKeyHash = 2;
constexpr uint8_t kKeyDown = 1;
constexpr uint8_t kKeyUp = 0;
}  // namespace pins

namespace {
using quota::uiMessage;
using quota::UiState;

constexpr uint32_t kSerialBaud = 115200;
constexpr uint32_t kI2cFrequency = 100000;
constexpr uint32_t kDebounceMs = 25;
constexpr uint32_t kIdleRedrawMs = 60000;
// 1px 进度条的帧间隔。I²C 约 1.4 KB/s，靠「只刷变化的两端」把单帧压到十几字节，
// 25 ms 一帧仍然留有充足余量；实际帧率由 I²C 花的时间决定。
constexpr uint32_t kLoadingFrameMs = 25;
constexpr uint32_t kMinimumValidEpoch = 1700000000UL;
// 设备在中国大陆：优先国内可达的 NTP 源（LWIP 最多 3 个），
// 不再使用在境内经常超时的 time.google.com。
constexpr char kNtpServer0[] = "ntp.aliyun.com";
constexpr char kNtpServer1[] = "ntp1.aliyun.com";
constexpr char kNtpServer2[] = "time.tencent.com";
constexpr uint32_t kInitialRetryDelaySeconds = 30;
constexpr uint32_t kMaximumRetryDelaySeconds = 15 * 60;
constexpr size_t kMaximumSerialCommandBytes = 64 * 1024;
constexpr char kClaudeClientId[] = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
constexpr char kClaudeTokenUrl[] = "https://platform.claude.com/v1/oauth/token";
constexpr char kClaudeUsageUrl[] = "https://api.anthropic.com/api/oauth/usage";
// ChatGPT（Codex CLI）用量：OAuth token 续期 + wham usage。
constexpr char kCodexClientId[] = "app_EMoamEEZ73f0CkXaXp7hrann";
constexpr char kCodexTokenUrl[] = "https://auth.openai.com/oauth/token";
constexpr char kCodexUsageUrl[] = "https://chatgpt.com/backend-api/wham/usage";
// Antigravity（Google Cloud Code）用的是 Antigravity 自己的 installed-app 客户端，
// 它只能走回环重定向，不能走设备码（Google 只允许 "TVs and Limited Input devices"
// 类型的客户端用设备码）。
//
// client id / secret 被 GitHub push protection 当成 secret，所以不写进仓库：
// 构建时由 firmware/scripts/gen_antigravity_client.py 从本地配置
// cli/antigravity-client.json（gitignore）或环境变量生成 antigravity_client.h。
#include "antigravity_client.h"
using antigravity_client::kClientId;
using antigravity_client::kClientSecret;
constexpr char kAntigravityTokenUrl[] = "https://oauth2.googleapis.com/token";
// 配额与档位/余额两个只读接口；daily 与 prod 两个通道都要试（见 refreshAntigravityUsage）。
constexpr char kAntigravityQuotaPath[] = "/v1internal:retrieveUserQuotaSummary";
constexpr char kAntigravityPlanPath[] = "/v1internal:loadCodeAssist";
constexpr char kAntigravityDailyHost[] = "daily-cloudcode-pa.googleapis.com";
constexpr char kAntigravityProdHost[] = "cloudcode-pa.googleapis.com";
// 配额接口对 User-Agent 有门禁：只有 `antigravity/hub/<版本> <os>/<arch>` 这种
// Antigravity Hub 形态才会返回完整的 Gemini + Claude/GPT 四个桶；其它 UA（纯
// "antigravity"、antigravity/1.15.7、node 客户端 UA）只返回 3p-*（Claude/GPT）组，
// 或者直接 403。2026-11-25 在真实账号上实测。
constexpr char kAntigravityQuotaUserAgent[] = "antigravity/hub/2.23.0 windows/amd64";
// NVS 键名含结尾 NUL 最多 16 字节（NVS_KEY_NAME_MAX_SIZE），所以名字最多 15 个字符：
// "antigravity_auth" 有 16 个，putBytes 会返回 0，凭据永远写不进去。
constexpr char kAntigravityAuthKey[] = "antigravity";
static_assert(sizeof(kAntigravityAuthKey) <= 16, "NVS 键名（含 NUL）不能超过 16 字节");

struct ClaudeAuth {
  String provisionId;
  String accessToken;
  String refreshToken;
  uint64_t expiresAt = 0;
  // Persist before sending a rotating refresh token. An interrupted exchange must
  // not be replayed on restart: the server may already have consumed the token.
  bool pending = false;
  bool configured() const { return !accessToken.isEmpty() && !refreshToken.isEmpty(); }
};
ClaudeAuth claudeAuth;
uint32_t claudeBlockedUntil = 0;

// Antigravity 的凭据单独存在一个 NVS 值里（与 Claude / ChatGPT 同一套保护：
// 续期前先落 pending，成功后一次写入）。Google 不轮换这个客户端的 refresh token，
// 但仍然按「结果不确定就不再重放」处理，避免上游以后改变行为时静默失效。
struct AntigravityAuth {
  String provisionId;
  String account;  // 登录用的 Google 邮箱，只用于 `status` 展示
  String accessToken;
  String refreshToken;
  uint64_t expiresAt = 0;
  bool pending = false;
  bool needsLogin = false;
  bool configured() const { return !accessToken.isEmpty() && !refreshToken.isEmpty(); }
};
AntigravityAuth antigravityAuth;

extern const uint8_t rootca_crt_bundle_start[]
    asm("_binary_data_cert_x509_crt_bundle_bin_start");

U8G2_SSD1315_128X64_NONAME_F_SW_I2C display(
    U8G2_R0, pins::kI2cClock, pins::kI2cData, U8X8_PIN_NONE);
Preferences preferences;

// 主循环和动画任务都会碰显示屏与 u8g2 缓冲，必须串行。
// 行配置也由主循环写、动画任务读，所以写入同样走这把锁。
SemaphoreHandle_t displayMutex = nullptr;

// 屏幕行配置：来源、顺序、窗口；由 NVS 的 `rows` 字符串决定。
quota::RowConfig rowConfigs[quota::kMaxRows];
size_t rowCount = 0;

struct Settings {
  String wifiSsid;
  String wifiPassword;
  String accessToken;
  String refreshToken;
  String accountId;
  String deepseekKeys;
  String openrouterKeys;
  // rows 控制「显示哪些来源」：形如 chatgpt:7d,deepseek,openrouter。
  // 没写进 rows 的来源不排队、不抓取、不上屏，凭据仍保留在 NVS。
  String rows;
  uint64_t expiresAt = 0;
  int16_t utcOffsetMinutes = 0;
  // ChatGPT 的续期状态跟随凭据一起持久化：pending 表示上一次续期结果不确定
  // （响应丢失/非可重试错误），needsLogin 表示上游明确拒绝，两种都不再自动重试。
  bool codexPending = false;
  bool codexNeedsLogin = false;
  // 最近一次安装 ChatGPT 凭据的串口 request_id；重发同一请求只确认、不重装，
  // 避免把已经被轮换掉的旧 refresh token 又写回来（Claude 用 provisionId 防同类问题）。
  String codexProvisionId;

  bool codexConfigured() const {
    return !accessToken.isEmpty() && !refreshToken.isEmpty() && !accountId.isEmpty();
  }

  bool deepseekConfigured() const { return !deepseekKeys.isEmpty(); }

  bool openrouterConfigured() const { return !openrouterKeys.isEmpty(); }

  bool hasCredentials(quota::RowSource source) const {
    switch (source) {
      case quota::RowSource::kChatgpt: return codexConfigured();
      case quota::RowSource::kClaude: return claudeAuth.configured();
      case quota::RowSource::kAntigravity: return antigravityAuth.configured();
      case quota::RowSource::kDeepseek: return deepseekConfigured();
      case quota::RowSource::kOpenrouter: return openrouterConfigured();
    }
    return false;
  }

  // 设备级配置：有 Wi-Fi，且至少一个写在 rows 里的来源有凭据。
  bool configured() const {
    if (wifiSsid.isEmpty() || rowCount == 0) return false;
    for (size_t i = 0; i < rowCount; ++i) {
      if (hasCredentials(rowConfigs[i].source)) return true;
    }
    return false;
  }
};

// 每个来源（Claude 用量、每个平台的余额）各自记录显示状态、数据新鲜度和排队时刻。
// 三个来源互不影响：一个失败不会拖住另一个的节奏，也不会共用一条错误信息。
struct SourceStatus {
  UiState state = UiState::kUnconfigured;
  String detail;
  uint32_t lastOkMs = 0;  // 最近一次成功获取的 millis()，0 表示还没有数据
  uint32_t dueAtMs = 0;   // 下一次自动刷新的时刻，0 表示不在队列里
  uint32_t retryDelaySeconds = kInitialRetryDelaySeconds;
};

// 一个平台按配置顺序保存多个 key 的余额；某个 key 失败时保留它上次成功值。
struct BalanceGroup {
  SourceStatus status;
  balance_protocol::Balance values[key_list::kMaxKeys];
  bool slotStale[key_list::kMaxKeys] = {};
  size_t count = 0;
  bool stale = false;

  bool anyPresent() const {
    for (size_t i = 0; i < count; ++i) {
      if (values[i].present) return true;
    }
    return false;
  }
};

using RateWindow = claude::RateWindow;

struct Key {
  const char* name;
  uint8_t pin;
  bool rawPressed;
  bool stablePressed;
  uint32_t changedAtMs;
};

Settings settings;
RateWindow fiveHour;
RateWindow sevenDay;
RateWindow chatgptFive;
RateWindow chatgptSeven;
RateWindow antigravitySeven;
// Antigravity 的档位与充值余额：只用于 `status` 展示和「额度用尽后显示余额」。
String antigravityPlan;
String antigravityProject;
antigravity::Credits antigravityCredits;
// 余额缓存的保鲜期：单次坏响应保留旧金额，但超过这个时长还没确认过就不再显示，
// 免得把很久以前的钱当成当前余额（3 个默认窗口刷新周期）。
constexpr uint32_t kAntigravityCreditsMaxAgeMs = 30 * 60 * 1000UL;
uint32_t antigravityCreditsOkMs = 0;  // 0 = 还没有成功拿到过余额
// 最近一次「200 但没有 gemini-weekly」时实际看到的 bucket id，只用于错误详情。
String debugAntigravityBuckets;
// 时钟未同步时 expiresAt 只能是 0（未知），用单调时钟限制主动续期频率：
// 至少 60 分钟才主动续一次；access token 真过期时走 401 触发的续期。
constexpr uint32_t kMinimumTokenRefreshMs = 60 * 60 * 1000UL;
uint32_t codexTokenRefreshMs = 0;
uint32_t claudeTokenRefreshMs = 0;
uint32_t antigravityTokenRefreshMs = 0;
SourceStatus claudeSource;
SourceStatus chatgptSource;
SourceStatus antigravitySource;
BalanceGroup deepseekBalances;
BalanceGroup openrouterBalances;

// 行配置与数据源之间的唯一映射，绘制、调度、串口字段都从这里取。
SourceStatus* statusFor(quota::RowSource source) {
  switch (source) {
    case quota::RowSource::kChatgpt: return &chatgptSource;
    case quota::RowSource::kClaude: return &claudeSource;
    case quota::RowSource::kAntigravity: return &antigravitySource;
    case quota::RowSource::kDeepseek: return &deepseekBalances.status;
    case quota::RowSource::kOpenrouter: return &openrouterBalances.status;
  }
  return nullptr;
}

bool rowEnabled(quota::RowSource source) {
  for (size_t i = 0; i < rowCount; ++i) {
    if (rowConfigs[i].source == source) return true;
  }
  return false;
}

// 没写进 rows 的来源不再抓取，也不该在 `status` 里继续报上一次的错误。
// 只清状态字段（数据本身留着），重新加回行时会立即抓一次。
void resetDisabledSourceStatus() {
  const quota::RowSource sources[] = {
      quota::RowSource::kChatgpt,     quota::RowSource::kClaude,
      quota::RowSource::kAntigravity, quota::RowSource::kDeepseek,
      quota::RowSource::kOpenrouter};
  for (quota::RowSource source : sources) {
    if (rowEnabled(source)) continue;
    SourceStatus* status = statusFor(source);
    if (status != nullptr) *status = SourceStatus{};
  }
}

// 动画任务在显示锁内读行配置；写入也必须持锁，避免读到「行数已变、内容还没换」的中间态。
void applyRowConfig(const quota::RowConfig* rows, size_t count) {
  if (displayMutex != nullptr) xSemaphoreTake(displayMutex, portMAX_DELAY);
  const size_t limit = count > quota::kMaxRows ? quota::kMaxRows : count;
  for (size_t i = 0; i < limit; ++i) rowConfigs[i] = rows[i];
  rowCount = limit;
  if (displayMutex != nullptr) xSemaphoreGive(displayMutex);
}
UiState uiState = UiState::kUnconfigured;
String lastErrorDetail;
Key keys[] = {
    {"K4(*)", pins::kKeyStar, false, false, 0},
    {"K3(#)", pins::kKeyHash, false, false, 0},
    {"K2(DOWN)", pins::kKeyDown, false, false, 0},
    {"K1(UP)", pins::kKeyUp, false, false, 0},
};
String serialLine;
String lastMutationId;
String lastMutationCommand;
String lastMutationReply;
uint32_t lastRefreshEpoch = 0;
uint32_t nextRedrawAtMs = 0;
uint8_t drawnPhase = 0;
bool refreshRequested = false;
bool sntpStarted = false;
// 哪几行正在抓取（bit0 Claude、bit1 DeepSeek、bit2 OpenRouter、bit3 ChatGPT、
// bit4 Antigravity）。这是 loading 动画唯一的开关：只在 refreshSources 里置位，
// 函数返回时统一清零，所以「刷新循环从未启动」的配置不会留下一条永远转动的线。
// 单字节写在单核上是原子的，读端在显示锁内读一次。
volatile uint8_t fetchingMask = 0;
constexpr uint8_t kFetchClaude = 1u << 0;
constexpr uint8_t kFetchDeepseek = 1u << 1;
constexpr uint8_t kFetchOpenrouter = 1u << 2;
constexpr uint8_t kFetchChatgpt = 1u << 3;
constexpr uint8_t kFetchAntigravity = 1u << 4;
constexpr uint8_t kDisplayI2cAddress = 0x3c;

uint32_t epochNow() {
  const time_t value = time(nullptr);
  return value >= static_cast<time_t>(kMinimumValidEpoch)
             ? static_cast<uint32_t>(value)
             : 0;
}

void setError(UiState state, const String& detail) {
  uiState = state;
  lastErrorDetail = detail;
}

// 成功：记下数据新鲜度，按该行配置的间隔排下一次。
void sourceSucceeded(SourceStatus& source, uint32_t intervalSeconds) {
  source.state = UiState::kReady;
  source.detail = "";
  source.lastOkMs = millis();
  source.retryDelaySeconds = kInitialRetryDelaySeconds;
  source.dueAtMs = millis() + intervalSeconds * 1000UL;
  lastRefreshEpoch = epochNow();
}

// 该来源在 rows 里的抓取间隔：同一来源占多行时取最短的。
uint32_t sourceInterval(quota::RowSource source) {
  return quota::sourceIntervalSeconds(rowConfigs, rowCount, source);
}

// 失败：按该来源自己的间隔做退避；Claude 还要尊重服务端的 Retry-After 冷却。
void scheduleSourceRetry(SourceStatus& source, uint32_t intervalSeconds) {
  if (&source == &claudeSource && claudeAuth.pending) {
    // 凭据已失效：不再自动重试，避免连续触发 token 轮换。
    source.dueAtMs = 0;
    return;
  }
  if (&source == &chatgptSource &&
      (settings.codexPending || settings.codexNeedsLogin)) {
    source.dueAtMs = 0;
    return;
  }
  if (&source == &antigravitySource &&
      (antigravityAuth.pending || antigravityAuth.needsLogin)) {
    source.dueAtMs = 0;
    return;
  }
  uint32_t wait = quota::retryDelay(intervalSeconds, source.retryDelaySeconds);
  if (&source == &claudeSource) {
    // 冷却截止时间是 Unix 时间；时钟未同步时不能拿它算等待（见 refresh_plan.h）。
    const uint32_t now = epochNow();
    wait = max(wait, quota::cooldownRemainingSeconds(now != 0, claudeBlockedUntil, now));
  }
  // millis 截止时间必须留在 32 位有符号比较范围内。
  wait = min(wait, static_cast<uint32_t>(7 * 86400));
  source.dueAtMs = millis() + wait * 1000UL;
  source.retryDelaySeconds = min(kMaximumRetryDelaySeconds, source.retryDelaySeconds * 2);
}

// 把这一行的失败状态记下来并安排退避；状态与详情由调用方给，避免依赖全局。
void sourceFailed(SourceStatus& source, uint32_t intervalSeconds, UiState state,
                  const String& detail) {
  source.state = state;
  source.detail = detail;
  scheduleSourceRetry(source, intervalSeconds);
}

// 数据确实更新了、但这一行仍要显示失败（例如多 key 里只坏了一个）。
// 调度完全复用成功路径，避免继承已经翻倍过的退避；展示状态再覆盖回异常。
void sourcePartiallySucceeded(SourceStatus& source, uint32_t intervalSeconds, UiState state,
                              const String& detail) {
  sourceSucceeded(source, intervalSeconds);
  source.state = state;
  source.detail = detail;
}

String wifiStatusDetail(wl_status_t status) {
  switch (status) {
    case WL_NO_SSID_AVAIL: return "找不到指定 WiFi（WL_NO_SSID_AVAIL）";
    case WL_CONNECT_FAILED: return "WiFi 连接被拒绝（WL_CONNECT_FAILED）";
    case WL_CONNECTION_LOST: return "WiFi 连接丢失（WL_CONNECTION_LOST）";
    case WL_DISCONNECTED: return "WiFi 未连接（WL_DISCONNECTED）";
    default: return "WiFi 状态码 " + String(static_cast<int>(status));
  }
}

bool resolveHost(const char* host) {
  IPAddress address;
  if (WiFi.hostByName(host, address) == 1) return true;
  setError(UiState::kDnsError, "DNS 解析失败：" + String(host));
  return false;
}

String transportDetail(const char* endpoint, int status, WiFiClientSecure& client) {
  char tlsError[128] = {};
  const int tlsCode = client.lastError(tlsError, sizeof(tlsError));
  String detail = String(endpoint) + " 传输失败（" + HTTPClient::errorToString(status) +
                  ", code " + String(status) + ")";
  if (tlsCode != 0 && tlsError[0] != '\0') {
    detail += "；TLS " + String(tlsError) + "（" + String(tlsCode) + ")";
  }
  return detail;
}

UiState transportState(int status, WiFiClientSecure& client) {
  if (status == HTTPC_ERROR_CONNECTION_REFUSED) return UiState::kTcpRefused;
  if (status == HTTPC_ERROR_READ_TIMEOUT) return UiState::kRequestTimeout;
  if (status == HTTPC_ERROR_NOT_CONNECTED || status == HTTPC_ERROR_CONNECTION_LOST) {
    return UiState::kConnectionLost;
  }
  char tlsError[4] = {};
  return client.lastError(tlsError, sizeof(tlsError)) != 0
             ? UiState::kTlsError
             : UiState::kTransportError;
}

UiState classifyHttpStatus(int status) {
  if (status == 401 || status == 403) return UiState::kAuthError;
  if (status == 429) return UiState::kRateLimitError;
  if (status >= 500) return UiState::kServerError;
  return UiState::kHttpError;
}

bool jsonString(cJSON* object, const char* key, String& destination);

bool saveClaudeAuth(const ClaudeAuth& auth) {
  cJSON* root = cJSON_CreateObject();
  if (!root) return false;
  bool complete = cJSON_AddStringToObject(root, "provision_id", auth.provisionId.c_str()) &&
      cJSON_AddStringToObject(root, "access_token", auth.accessToken.c_str()) &&
      cJSON_AddStringToObject(root, "refresh_token", auth.refreshToken.c_str()) &&
      cJSON_AddNumberToObject(root, "expires_at", static_cast<double>(auth.expiresAt)) &&
      cJSON_AddBoolToObject(root, "pending", auth.pending);
  char* encoded = complete ? cJSON_PrintUnformatted(root) : nullptr;
  // One NVS value commits the token pair and expiry together; never mix generations.
  bool ok = encoded && preferences.putString("claude_auth", encoded) == strlen(encoded);
  if (encoded) cJSON_free(encoded);
  cJSON_Delete(root);
  return ok;
}

void loadClaudeAuth() {
  const String encoded = preferences.getString("claude_auth");
  cJSON* root = cJSON_Parse(encoded.c_str());
  if (root) {
    ClaudeAuth candidate;
    cJSON* expires = cJSON_GetObjectItemCaseSensitive(root, "expires_at");
    // expires_at == 0 表示「未知」：时钟未同步时续期会写 0，重启后不能因为
    // 判断不了绝对时间就把整份凭据丢掉。
    const bool expiresValid =
        cJSON_IsNumber(expires) &&
        (expires->valuedouble == 0 ||
         claude::integer(expires, kMinimumValidEpoch, UINT32_MAX));
    if (jsonString(root, "access_token", candidate.accessToken) &&
        jsonString(root, "refresh_token", candidate.refreshToken) && expiresValid &&
        candidate.configured()) {
      candidate.expiresAt = static_cast<uint64_t>(expires->valuedouble);
      jsonString(root, "provision_id", candidate.provisionId);
      candidate.pending = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "pending"));
      claudeAuth = candidate;
    }
    cJSON_Delete(root);
  }
  claudeBlockedUntil = preferences.getUInt("claude_wait", 0);
}

// ChatGPT 凭据集中保存在单个 NVS 值里：一次写入，断电不会留下 access 与
// refresh 不同代。旧的分散 key 只在迁移时读取。
void loadCodexAuth() {
  // 旧固件把凭据拆成 access/refresh/account/expires。只有这一组**完整**时才说明
  // 有人用旧固件重新登录过（降级再升级），以它为准并重新迁移；迁移中断或旧固件
  // 只写 expires 之类的残缺记录不能遮住完整的新记录。
  const bool legacyComplete = !settings.accessToken.isEmpty() &&
                              !settings.refreshToken.isEmpty() &&
                              !settings.accountId.isEmpty();
  if (legacyComplete) return;
  // 优先读 blob；早期构建把它写成了 NVS string，读不到 blob 时回退。
  size_t length = preferences.getBytesLength("chatgpt_auth");
  String stored;
  if (length > 0) {
    char* buffer = static_cast<char*>(malloc(length + 1));
    if (buffer == nullptr) return;
    const size_t read = preferences.getBytes("chatgpt_auth", buffer, length);
    buffer[read] = '\0';
    stored = buffer;
    free(buffer);
  } else {
    stored = preferences.getString("chatgpt_auth");
  }
  if (stored.isEmpty()) return;
  cJSON* root = cJSON_ParseWithOpts(stored.c_str(), nullptr, true);
  if (root == nullptr) return;
  String access, refresh, account;
  const cJSON* expires = cJSON_GetObjectItemCaseSensitive(root, "expires");
  if (jsonString(root, "access", access) && !access.isEmpty() &&
      jsonString(root, "refresh", refresh) && !refresh.isEmpty() &&
      jsonString(root, "account", account)) {
    settings.accessToken = access;
    settings.refreshToken = refresh;
    settings.accountId = account;
    settings.expiresAt =
        cJSON_IsNumber(expires) ? static_cast<uint64_t>(expires->valuedouble) : 0;
    settings.codexPending =
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "pending"));
    settings.codexNeedsLogin =
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "needs_login"));
    String provision;
    if (jsonString(root, "provision", provision)) settings.codexProvisionId = provision;
  }
  cJSON_Delete(root);
}

bool saveCodexAuth() {
  if (settings.accessToken.isEmpty() && settings.refreshToken.isEmpty() &&
      settings.accountId.isEmpty()) {
    preferences.remove("chatgpt_auth");
    return true;
  }
  cJSON* root = cJSON_CreateObject();
  if (root == nullptr) return false;
  // 单值写入只保证「一起提交」，不保证字段齐全：任一字段分配失败都不能落盘。
  const bool complete =
      cJSON_AddStringToObject(root, "access", settings.accessToken.c_str()) != nullptr &&
      cJSON_AddStringToObject(root, "refresh", settings.refreshToken.c_str()) != nullptr &&
      cJSON_AddStringToObject(root, "account", settings.accountId.c_str()) != nullptr &&
      cJSON_AddNumberToObject(root, "expires", static_cast<double>(settings.expiresAt)) !=
          nullptr &&
      cJSON_AddBoolToObject(root, "pending", settings.codexPending) != nullptr &&
      cJSON_AddBoolToObject(root, "needs_login", settings.codexNeedsLogin) != nullptr &&
      cJSON_AddStringToObject(root, "provision", settings.codexProvisionId.c_str()) != nullptr;
  if (!complete) {
    cJSON_Delete(root);
    return false;
  }
  char* text = cJSON_PrintUnformatted(root);
  bool ok = text != nullptr;
  if (ok) {
    // 用 blob 而不是 NVS string：string 单值上限约 4000 B，长 token 会写不进去。
    const size_t length = strlen(text);
    ok = length > 0 && preferences.putBytes("chatgpt_auth", text, length) == length;
    cJSON_free(text);
  }
  cJSON_Delete(root);
  if (!ok) return false;
  // 迁移到单值后删掉旧的分散 key，避免两套记录混用。
  preferences.remove("access");
  preferences.remove("refresh");
  preferences.remove("account");
  preferences.remove("expires");
  return true;
}

// Antigravity 凭据：与 ChatGPT / Claude 一样整份存成单个 NVS 值（blob），
// 续期前先写 pending，成功后一次提交，断电不会留下半代凭据。
bool saveAntigravityAuth(const AntigravityAuth& auth) {
  if (!auth.configured()) {
    preferences.remove(kAntigravityAuthKey);
    return true;
  }
  cJSON* root = cJSON_CreateObject();
  if (root == nullptr) return false;
  const bool complete =
      cJSON_AddStringToObject(root, "access", auth.accessToken.c_str()) != nullptr &&
      cJSON_AddStringToObject(root, "refresh", auth.refreshToken.c_str()) != nullptr &&
      cJSON_AddStringToObject(root, "account", auth.account.c_str()) != nullptr &&
      cJSON_AddNumberToObject(root, "expires", static_cast<double>(auth.expiresAt)) != nullptr &&
      cJSON_AddBoolToObject(root, "pending", auth.pending) != nullptr &&
      cJSON_AddBoolToObject(root, "needs_login", auth.needsLogin) != nullptr &&
      cJSON_AddStringToObject(root, "provision", auth.provisionId.c_str()) != nullptr;
  if (!complete) {
    cJSON_Delete(root);
    return false;
  }
  char* text = cJSON_PrintUnformatted(root);
  bool ok = text != nullptr;
  if (ok) {
    const size_t length = strlen(text);
    ok = length > 0 && preferences.putBytes(kAntigravityAuthKey, text, length) == length;
    cJSON_free(text);
  }
  cJSON_Delete(root);
  return ok;
}

void loadAntigravityAuth() {
  size_t length = preferences.getBytesLength(kAntigravityAuthKey);
  if (length == 0) return;
  char* buffer = static_cast<char*>(malloc(length + 1));
  if (buffer == nullptr) return;
  const size_t read = preferences.getBytes(kAntigravityAuthKey, buffer, length);
  buffer[read] = '\0';
  String stored = buffer;
  free(buffer);
  cJSON* root = cJSON_ParseWithOpts(stored.c_str(), nullptr, true);
  if (root == nullptr) return;
  AntigravityAuth candidate;
  const cJSON* expires = cJSON_GetObjectItemCaseSensitive(root, "expires");
  // expires == 0 表示「未知」（时钟未同步时续期写不下绝对时间），不能因此丢凭据。
  const bool expiresValid =
      cJSON_IsNumber(expires) &&
      (expires->valuedouble == 0 ||
       claude::integer(expires, kMinimumValidEpoch, UINT32_MAX));
  if (jsonString(root, "access", candidate.accessToken) &&
      jsonString(root, "refresh", candidate.refreshToken) && expiresValid &&
      candidate.configured()) {
    candidate.expiresAt = static_cast<uint64_t>(expires->valuedouble);
    jsonString(root, "account", candidate.account);
    jsonString(root, "provision", candidate.provisionId);
    candidate.pending = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "pending"));
    candidate.needsLogin =
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "needs_login"));
    antigravityAuth = candidate;
  }
  cJSON_Delete(root);
}

void resetBalanceGroup(BalanceGroup& group, const String& rawKeys) {
  group = BalanceGroup{};
  key_list::List keys;
  if (key_list::parse(rawKeys.c_str(), rawKeys.length(), keys) && keys.count > 0) {
    group.count = keys.count;
  }
}

void loadSettings() {
  preferences.begin("codexquota", false);
  loadClaudeAuth();
  settings.wifiSsid = preferences.getString("wifi_ssid");
  settings.wifiPassword = preferences.getString("wifi_pass");
  settings.accessToken = preferences.getString("access");
  settings.refreshToken = preferences.getString("refresh");
  settings.accountId = preferences.getString("account");
  settings.deepseekKeys = preferences.getString("deepseek");
  settings.openrouterKeys = preferences.getString("openrouter");
  settings.expiresAt = preferences.getULong64("expires", 0);
  loadCodexAuth();
  loadAntigravityAuth();
  // 旧固件把 ChatGPT 凭据拆成 access/refresh/account/expires 四个 key；当它们组成
  // 完整的旧记录时（首次升级，或降级后旧固件又写过），以它们为准重新迁到单值存储，
  // 避免单值里留着更旧的 token；残缺残留不参与判断，随迁移一起清掉。
  if (settings.codexConfigured() &&
      (preferences.isKey("access") || preferences.isKey("refresh") ||
       preferences.isKey("account") || preferences.isKey("expires"))) {
    saveCodexAuth();
  }
  settings.utcOffsetMinutes = preferences.getShort("utc_off", 0);
  settings.rows = preferences.getString("rows");
  quota::RowConfig loadedRows[quota::kMaxRows];
  size_t loadedCount = 0;
  if (!quota::parseRows(settings.rows.c_str(), settings.rows.length(), loadedRows, loadedCount)) {
    // 没写过或写坏的旧值都回到默认：第一行 ChatGPT 7 天，再加两个余额。
    settings.rows = quota::kDefaultRows;
    quota::parseRows(settings.rows.c_str(), settings.rows.length(), loadedRows, loadedCount);
  }
  applyRowConfig(loadedRows, loadedCount);
  resetBalanceGroup(deepseekBalances, settings.deepseekKeys);
  resetBalanceGroup(openrouterBalances, settings.openrouterKeys);
}

bool saveSettings() {
  bool ok = true;
  // login may be used before set: an empty SSID is a valid intermediate state
  // and must not make an otherwise successful credential write look like a
  // failure.
  if (!settings.wifiSsid.isEmpty()) {
    ok &= preferences.putString("wifi_ssid", settings.wifiSsid) > 0;
  } else {
    preferences.remove("wifi_ssid");
  }
  preferences.putString("wifi_pass", settings.wifiPassword);
  ok &= saveCodexAuth();
  if (!settings.deepseekKeys.isEmpty()) {
    ok &= preferences.putString("deepseek", settings.deepseekKeys) > 0;
  } else {
    preferences.remove("deepseek");
  }
  if (!settings.openrouterKeys.isEmpty()) {
    ok &= preferences.putString("openrouter", settings.openrouterKeys) > 0;
  } else {
    preferences.remove("openrouter");
  }
  ok &= preferences.putShort("utc_off", settings.utcOffsetMinutes) > 0;
  if (!settings.rows.isEmpty()) ok &= preferences.putString("rows", settings.rows) > 0;
  return ok;
}

void clearSettings() {
  preferences.clear();
  settings = Settings{};
  claudeAuth = ClaudeAuth{};
  claudeBlockedUntil = 0;
  claudeSource = SourceStatus{};
  antigravityAuth = AntigravityAuth{};
  antigravitySource = SourceStatus{};
  antigravitySeven = RateWindow{};
  antigravityPlan = "";
  antigravityProject = "";
  antigravityCredits = antigravity::Credits{};
  antigravityCreditsOkMs = 0;
  deepseekBalances = BalanceGroup{};
  openrouterBalances = BalanceGroup{};
  fiveHour = RateWindow{};
  sevenDay = RateWindow{};
  chatgptFive = RateWindow{};
  chatgptSeven = RateWindow{};
  chatgptSource = SourceStatus{};
  codexTokenRefreshMs = 0;
  claudeTokenRefreshMs = 0;
  antigravityTokenRefreshMs = 0;
  // 清空后回到默认行配置（ChatGPT 7d + 两个余额），凭据已经一起清掉。
  settings.rows = quota::kDefaultRows;
  quota::RowConfig defaultRows[quota::kMaxRows];
  size_t defaultCount = 0;
  if (quota::parseRows(settings.rows.c_str(), settings.rows.length(), defaultRows, defaultCount)) {
    applyRowConfig(defaultRows, defaultCount);
  }
  lastRefreshEpoch = 0;
  refreshRequested = false;
  WiFi.disconnect(true, true);
  uiState = UiState::kUnconfigured;
  lastErrorDetail = "";
}

String formEncode(const String& input) {
  static const char hex[] = "0123456789ABCDEF";
  String output;
  output.reserve(input.length() * 11 / 10 + 8);
  for (size_t i = 0; i < input.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(input[i]);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      output += static_cast<char>(c);
    } else {
      output += '%';
      output += hex[c >> 4];
      output += hex[c & 0x0F];
    }
  }
  return output;
}

void ensureTimeSync();

bool connectWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    uiState = UiState::kConnecting;
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(settings.wifiSsid.c_str(), settings.wifiPassword.c_str());
    const uint32_t deadline = millis() + 15000;
    while (WiFi.status() != WL_CONNECTED &&
           static_cast<int32_t>(millis() - deadline) < 0) {
      delay(100);
    }
    if (WiFi.status() != WL_CONNECTED) {
      const wl_status_t status = WiFi.status();
      const UiState state = status == WL_NO_SSID_AVAIL
                                ? UiState::kWifiNoNetwork
                                : status == WL_CONNECT_FAILED
                                      ? UiState::kWifiConnectionFailed
                                      : status == WL_CONNECTION_LOST
                                            ? UiState::kWifiDisconnected
                                            : UiState::kWifiTimeout;
      setError(state, wifiStatusDetail(status));
      return false;
    }
  }
  ensureTimeSync();
  return true;
}

// SNTP 常驻轮询：只在尚未生效时启动一次，期间不阻塞；同步成功后由设备
// 自身 RTC 走时。这里不再每轮调用 configTime，避免把刚同步好的结果重置掉。
// 时间暂不可用不阻止抓取：按日期展示的内容等时钟就绪后自然恢复。
void ensureTimeSync() {
  if (epochNow() != 0) return;
  if (sntpStarted) return;
  sntpStarted = true;
  configTime(0, 0, kNtpServer0, kNtpServer1, kNtpServer2);
  sntp_set_sync_interval(3600 * 1000);  // 每小时校正一次走时漂移
}

bool jsonString(cJSON* object, const char* key, String& destination) {
  cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
  if (!cJSON_IsString(value) || value->valuestring == nullptr) return false;
  destination = value->valuestring;
  return true;
}

void recordClaudeCooldown(HTTPClient& http, int status) {
  // 冷却截止时间用 Unix 时间表示，时钟还没同步时记录下来没有意义。
  if (epochNow() == 0) return;
  const String retryAfter = http.header("Retry-After");
  const uint32_t requested = claude::retryAfter(retryAfter.c_str(), epochNow());
  if (status != 429 && requested == 0) return;
  uint32_t delay = max(requested, static_cast<uint32_t>(status == 429 ? 300 : 30));
  const uint64_t until = static_cast<uint64_t>(epochNow()) + delay;
  claudeBlockedUntil = until > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(until);
  preferences.putUInt("claude_wait", claudeBlockedUntil);
}

bool refreshClaudeToken() {
  if (claudeAuth.pending) {
    setError(UiState::kAuthError, "Claude 续期结果不确定或凭据失效，请重新运行 login");
    return false;
  }
  if (!resolveHost("platform.claude.com")) return false;
  WiFiClientSecure client;
  client.setCACertBundle(rootca_crt_bundle_start);
  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(30000);
  if (!http.begin(client, kClaudeTokenUrl)) {
    setError(UiState::kTransportError, "Claude OAuth 请求初始化失败");
    return false;
  }
  const char* headers[] = {"Retry-After"};
  http.collectHeaders(headers, 1);
  http.addHeader("Accept", "application/json");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  const String body = "grant_type=refresh_token&client_id=" + formEncode(kClaudeClientId) +
                      "&refresh_token=" + formEncode(claudeAuth.refreshToken);
  // 记录「尝试」时间，时钟未同步时用它限流。
  claudeTokenRefreshMs = millis();
  ClaudeAuth pending = claudeAuth;
  pending.pending = true;
  if (!saveClaudeAuth(pending)) {
    http.end();
    setError(UiState::kConfigError, "无法保存 Claude 续期状态，未发送请求");
    return false;
  }
  claudeAuth = pending;
  const int status = http.POST(body);
  const String response = status == 200 ? http.getString() : String();
  recordClaudeCooldown(http, status);
  http.end();
  if (status <= 0) {
    setError(UiState::kAuthError, "Claude OAuth 续期响应丢失，请重新运行 login");
    return false;
  }
  if (status != 200) {
    // Only explicit retryable rejections permit reusing this refresh token.
    if (status == 429 || status >= 500) {
      pending.pending = false;
      if (saveClaudeAuth(pending)) claudeAuth = pending;
    }
    setError(claudeAuth.pending ? UiState::kAuthError : classifyHttpStatus(status),
             "Claude OAuth 服务返回 HTTP " + String(status));
    return false;
  }
  cJSON* root = cJSON_ParseWithOpts(response.c_str(), nullptr, true);
  ClaudeAuth candidate;
  candidate.provisionId = claudeAuth.provisionId;
  cJSON* expires = cJSON_GetObjectItemCaseSensitive(root, "expires_in");
  bool valid = jsonString(root, "access_token", candidate.accessToken) &&
      jsonString(root, "refresh_token", candidate.refreshToken) && candidate.configured() &&
      claude::integer(expires, 1, 365 * 86400);
  if (valid) {
    candidate.expiresAt = claude::expiryFor(
        epochNow(), static_cast<uint32_t>(expires->valuedouble));
  }
  cJSON_Delete(root);
  if (!valid) {
    setError(UiState::kAuthError, "Claude OAuth 续期响应无效，请重新运行 login");
    return false;
  }
  if (!saveClaudeAuth(candidate)) {
    setError(UiState::kAuthError, "Claude 新凭据写入失败，请重新运行 login");
    return false;
  }
  claudeAuth = candidate;
  return true;
}

int getClaudeUsage(String& response) {
  if (!resolveHost("api.anthropic.com")) return -1;
  WiFiClientSecure client;
  client.setCACertBundle(rootca_crt_bundle_start);
  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(15000);
  if (!http.begin(client, kClaudeUsageUrl)) {
    setError(UiState::kTransportError, "Claude 用量请求初始化失败");
    return -1;
  }
  const char* headers[] = {"Retry-After"};
  http.collectHeaders(headers, 1);
  http.addHeader("Accept", "application/json");
  http.addHeader("Authorization", "Bearer " + claudeAuth.accessToken);
  http.addHeader("anthropic-beta", "oauth-2025-04-20");
  const int status = http.GET();
  if (status == 200) response = http.getString();
  recordClaudeCooldown(http, status);
  http.end();
  if (status <= 0) setError(transportState(status, client), transportDetail("Claude 用量", status, client));
  return status;
}

bool refreshClaudeUsage() {
  if (claudeAuth.pending) {
    setError(UiState::kAuthError, "Claude 凭据需要重新授权，请运行 login");
    return false;
  }
  // Manual refresh and reboot must respect the same server cooldown.
  // 时钟未就绪时冷却截止时间不可解释，按没有冷却处理，否则冷启动会卡死。
  const uint32_t nowEpoch = epochNow();
  if (quota::cooldownActive(nowEpoch != 0, claudeBlockedUntil, nowEpoch)) {
    setError(UiState::kRateLimitError, "Claude 接口冷却中，请稍后重试");
    return false;
  }
  bool refreshed = false;
  bool tokenDue = false;
  if (nowEpoch == 0) {
    tokenDue = claudeTokenRefreshMs == 0 ||
               millis() - claudeTokenRefreshMs >= kMinimumTokenRefreshMs;
  } else {
    tokenDue = claudeAuth.expiresAt == 0 ||
               claudeAuth.expiresAt <= static_cast<uint64_t>(nowEpoch) + 120;
  }
  if (tokenDue) {
    if (!refreshClaudeToken()) return false;
    refreshed = true;
  }
  String response;
  int status = getClaudeUsage(response);
  if (status == 401 && !refreshed) {
    // 刚续期过就先不连环续期，避免时钟未同步/上游异常时反复打 token 端点。
    if (claudeTokenRefreshMs != 0 && millis() - claudeTokenRefreshMs < 60 * 1000UL) {
      setError(UiState::kAuthError, "Claude 凭据验证失败，请稍后重试或重新 login");
      return false;
    }
    if (!refreshClaudeToken()) return false;
    status = getClaudeUsage(response);
  }
  if (status != 200) {
    if (status > 0) {
      setError(classifyHttpStatus(status), "Claude 用量接口返回 HTTP " + String(status));
      if (status == 401 || status == 403) {
        // New credentials rejected, or scope missing: stop repeated token rotation.
        // 状态必须落盘成功，否则重启后可能重放已经失效的 refresh token。
        claudeAuth.pending = true;
        if (!saveClaudeAuth(claudeAuth)) {
          setError(UiState::kConfigError, "无法保存 Claude 需要重新登录的状态");
          return false;
        }
      }
    }
    return false;
  }
  if (!claude::parseUsage(response.c_str(), fiveHour, sevenDay)) {
    setError(UiState::kDataError, "Claude 用量响应缺少有效的 5h/7d 数据");
    return false;
  }
  sourceSucceeded(claudeSource, sourceInterval(quota::RowSource::kClaude));
  return true;
}

// ChatGPT（Codex CLI）OAuth 续期：refresh_token 换 access_token；服务端可能同时
// 轮换 refresh token。和 Claude 一样，发请求前先把「进行中」落盘：如果响应丢失或
// 掉电，下次不再重放可能已被上游消费的 refresh token，而是要求重新登录。
bool refreshChatgptToken() {
  if (settings.codexPending) {
    setError(UiState::kAuthError, "ChatGPT 续期结果不确定，请重新运行 login");
    return false;
  }
  if (!resolveHost("auth.openai.com")) return false;
  WiFiClientSecure client;
  client.setCACertBundle(rootca_crt_bundle_start);
  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(30000);
  if (!http.begin(client, kCodexTokenUrl)) {
    setError(UiState::kTransportError, "ChatGPT OAuth 请求初始化失败");
    return false;
  }
  http.addHeader("Accept", "application/json");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  const String body = "grant_type=refresh_token&client_id=" + formEncode(kCodexClientId) +
                      "&refresh_token=" + formEncode(settings.refreshToken);
  // 记录「尝试」时间（不管成功与否），统一限制续期频率。
  codexTokenRefreshMs = millis();
  Settings pending = settings;
  pending.codexPending = true;
  settings = pending;
  if (!saveCodexAuth()) {
    http.end();
    settings.codexPending = false;
    setError(UiState::kConfigError, "无法保存 ChatGPT 续期状态，未发送请求");
    return false;
  }
  const int status = http.POST(body);
  const String response = status == 200 ? http.getString() : String();
  http.end();
  if (status <= 0) {
    // 请求可能已经被上游处理：保留 pending，要求重新登录，避免重放旧 token。
    settings = pending;
    saveCodexAuth();
    setError(UiState::kAuthError, "ChatGPT OAuth 续期响应丢失，请重新运行 login");
    return false;
  }
  if (status != 200) {
    // 只有明确可重试的拒绝才允许继续使用旧 refresh token；清除 pending 必须
    // 先落盘成功，否则 RAM 说「可重试」而 NVS 仍停在「不确定」，重启后会误锁。
    if (status == 429 || status >= 500) {
      pending.codexPending = false;
      Settings previousPending = settings;
      settings = pending;
      if (!saveCodexAuth()) settings = previousPending;
    }
    setError(settings.codexPending ? UiState::kAuthError : classifyHttpStatus(status),
             "ChatGPT OAuth 服务返回 HTTP " + String(status));
    return false;
  }
  cJSON* root = cJSON_ParseWithOpts(response.c_str(), nullptr, true);
  const cJSON* expires = cJSON_GetObjectItemCaseSensitive(root, "expires_in");
  Settings candidate = settings;
  const bool valid = jsonString(root, "access_token", candidate.accessToken) &&
                     !candidate.accessToken.isEmpty() &&
                     claude::integer(expires, 1, 365 * 86400);
  String rotatedRefresh;
  if (valid && jsonString(root, "refresh_token", rotatedRefresh) &&
      !rotatedRefresh.isEmpty()) {
    candidate.refreshToken = rotatedRefresh;
  }
  if (valid) {
    // 时钟还没同步时记「未知」（0），并用单调时钟限制续期频率（见 refreshChatgptUsage）。
    candidate.expiresAt = claude::expiryFor(
        epochNow(), static_cast<uint32_t>(expires->valuedouble));
  }
  cJSON_Delete(root);
  if (!valid) {
    setError(UiState::kAuthError, "ChatGPT OAuth 续期响应无效，请重新运行 login");
    return false;
  }
  candidate.codexPending = false;
  candidate.codexNeedsLogin = false;
  const Settings previous = settings;
  settings = candidate;
  if (!saveCodexAuth()) {
    settings = previous;
    setError(UiState::kAuthError, "ChatGPT 新凭据写入失败，请重新运行 login");
    return false;
  }
  return true;
}

int getChatgptUsage(String& response) {
  if (!resolveHost("chatgpt.com")) return -1;
  WiFiClientSecure client;
  client.setCACertBundle(rootca_crt_bundle_start);
  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(15000);
  if (!http.begin(client, kCodexUsageUrl)) {
    setError(UiState::kTransportError, "ChatGPT 用量请求初始化失败");
    return -1;
  }
  http.addHeader("Accept", "application/json");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + settings.accessToken);
  http.addHeader("Chatgpt-Account-Id", settings.accountId);
  http.addHeader("User-Agent", "codex_cli_rs/0.76.0 (ESP32-C3; quota-display)");
  const int status = http.GET();
  if (status == 200) response = http.getString();
  http.end();
  if (status <= 0) {
    setError(transportState(status, client), transportDetail("ChatGPT 用量", status, client));
  }
  return status;
}

bool refreshChatgptUsage() {
  // 续期结果不确定或被上游拒绝时不再自动重试，避免反复轮换 token。
  if (settings.codexPending || settings.codexNeedsLogin) {
    setError(UiState::kAuthError, "ChatGPT 凭据需要重新授权，请运行 login");
    return false;
  }
  bool refreshed = false;
  const uint32_t nowEpoch = epochNow();
  bool tokenDue = false;
  if (nowEpoch == 0) {
    tokenDue = codexTokenRefreshMs == 0 ||
               millis() - codexTokenRefreshMs >= kMinimumTokenRefreshMs;
  } else {
    tokenDue = settings.expiresAt == 0 ||
               settings.expiresAt <= static_cast<uint64_t>(nowEpoch) + 120;
  }
  if (tokenDue) {
    if (!refreshChatgptToken()) return false;
    refreshed = true;
  }
  String response;
  int status = getChatgptUsage(response);
  if (status == 401 && !refreshed) {
    // 刚续期过（例如 401 紧跟一次续期尝试）就先不连环续期，避免在时钟未同步
    // 或上游异常时按抓取间隔反复打 token 端点。
    if (codexTokenRefreshMs != 0 && millis() - codexTokenRefreshMs < 60 * 1000UL) {
      setError(UiState::kAuthError, "ChatGPT 凭据验证失败，请稍后重试或重新 login");
      return false;
    }
    if (!refreshChatgptToken()) return false;
    status = getChatgptUsage(response);
  }
  if (status != 200) {
    if (status > 0) {
      setError(classifyHttpStatus(status), "ChatGPT 用量接口返回 HTTP " + String(status));
      if (status == 401 || status == 403) {
        // 续期后仍被拒绝：停止自动续期，等用户重新 login。
        settings.codexNeedsLogin = true;
        saveCodexAuth();
      }
    }
    return false;
  }
  if (!codex::parseUsage(response.c_str(), chatgptFive, chatgptSeven)) {
    setError(UiState::kDataError, "ChatGPT 用量响应缺少有效的 5h/7d 数据");
    return false;
  }
  sourceSucceeded(chatgptSource, sourceInterval(quota::RowSource::kChatgpt));
  return true;
}

// Antigravity（Google Cloud Code）只读请求：JSON + Bearer。返回 HTTP 状态，
// 传输失败返回 <=0 并记下错误详情。
// User-Agent 是必要的：cloudcode-pa 对不认识的 UA 直接 403（上游行为，见 README）。
int postAntigravity(const char* host, const char* path, const char* userAgent,
                    const String& body, String& response,
                    const char* apiClient = nullptr) {
  if (!resolveHost(host)) return -1;
  WiFiClientSecure client;
  client.setCACertBundle(rootca_crt_bundle_start);
  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(15000);
  const String url = String("https://") + host + path;
  if (!http.begin(client, url)) {
    setError(UiState::kTransportError, "Antigravity 请求初始化失败");
    return -1;
  }
  http.addHeader("Accept", "application/json");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + antigravityAuth.accessToken);
  // HTTPClient::addHeader() 会静默忽略 User-Agent（它只认 setUserAgent），
  // 而上游会按 UA 拒绝不认识的客户端，所以必须走 setUserAgent。
  http.setUserAgent(userAgent);
  if (apiClient != nullptr) {
    http.addHeader("X-Goog-Api-Client", apiClient);
  }
  const int status = http.POST(body);
  if (status == 200) response = http.getString();
  http.end();
  if (status <= 0) {
    setError(transportState(status, client), transportDetail("Antigravity", status, client));
  }
  return status;
}

// 配额摘要：daily 与 prod 两个通道都试，取能解析出 gemini-weekly 的那份。
// 公开证据对哪个通道权威互相矛盾（一份说 daily 缺 Gemini 组，另一份说 prod 会
// 卡在过期窗口），所以先 daily（与生成请求同路），缺组时再问 prod。
enum class QuotaFetch : uint8_t {
  kOk,           // 拿到了可用的每周窗口
  kAuthRejected, // 两个通道都明确 401/403：令牌或账号问题
  kBadData,      // 有 200 响应但里面没有 gemini-weekly
  kTransient,    // 传输失败 / 5xx / 一台拒绝另一台不可用
};

QuotaFetch fetchAntigravityQuota(RateWindow& seven) {
  // 档位响应里带 cloudaicompanionProject；带上它查询可以让账号落在自己的额度池。
  // 用 cJSON 构造 body：项目 id 来自上游，手拼字符串遇到引号/反斜杠会发出坏 JSON。
  String body = "{}";
  if (!antigravityProject.isEmpty()) {
    cJSON* root = cJSON_CreateObject();
    if (root != nullptr &&
        cJSON_AddStringToObject(root, "project", antigravityProject.c_str()) != nullptr) {
      char* encoded = cJSON_PrintUnformatted(root);
      if (encoded != nullptr) {
        body = encoded;
        cJSON_free(encoded);
      }
    }
    cJSON_Delete(root);
  }
  const char* hosts[] = {kAntigravityDailyHost, kAntigravityProdHost};
  int authRejections = 0;
  int hostCount = 0;
  int lastStatus = -1;
  bool sawHttp200 = false;
  String lastResponse;
  for (const char* host : hosts) {
    ++hostCount;
    String response;
    const int status =
        postAntigravity(host, kAntigravityQuotaPath, kAntigravityQuotaUserAgent, body, response);
    if (status == 401 || status == 403) {
      // 一台拒绝不代表令牌失效（可能是这台主机的 UA/灰度差异），继续问另一台。
      ++authRejections;
      lastStatus = status;
      continue;
    }
    if (status != 200) {
      lastStatus = status;
      continue;
    }
    sawHttp200 = true;
    lastStatus = 200;
    RateWindow candidate;
    if (antigravity::parseQuotaSummary(response.c_str(), candidate)) {
      seven = candidate;
      return QuotaFetch::kOk;
    }
    // 只有解析失败才留一份响应做诊断摘要，成功路径不做这次复制。
    lastResponse = response;
  }
  if (authRejections > 0 && authRejections == hostCount) return QuotaFetch::kAuthRejected;
  if (sawHttp200) {
    // 把实际看到的 bucket id 带进错误详情，方便判断是上游换了 id 还是这台账号
    // 根本没有 Gemini 组。
    char described[64] = {};
    antigravity::describeBuckets(lastResponse.c_str(), described, sizeof(described));
    debugAntigravityBuckets = described;
    return QuotaFetch::kBadData;
  }
  if (authRejections > 0) {
    // 只有一台拒绝、另一台不可用：不是登录失效，别把状态词写成「登录失效」。
    setError(UiState::kHttpError, "Antigravity 接口暂时不可用");
  } else if (lastStatus > 0) {
    setError(classifyHttpStatus(lastStatus), "Antigravity 接口返回 HTTP " + String(lastStatus));
  }
  return QuotaFetch::kTransient;
}

// 档位与充值余额：先 daily（与 agy 的档位查询一致），拿不到充值余额再问 prod。
//
// 充值余额决定「额度用尽后这一行显示什么」，是必须信息；档位只在 `status` 里展示。
// 所以只要还没解析出 credits 就继续问下一台主机，而不是拿到档位就返回。
//
// 三种结果分开处理：找到 → 采用；合法响应里明确没有 → 清空（屏幕显示 `--`）；
// 响应本身无效（空/被截断/不是 JSON）→ 保留旧值，等下一次抓取。
bool fetchAntigravityPlanAndCredits() {
  const char* hosts[] = {kAntigravityDailyHost, kAntigravityProdHost};
  bool sawAbsent = false;
  for (const char* host : hosts) {
    String response;
    if (postAntigravity(host, kAntigravityPlanPath, kAntigravityQuotaUserAgent, "{}", response) !=
        200) {
      continue;
    }
    char plan[48] = {};
    if (antigravity::parsePlan(response.c_str(), plan, sizeof(plan))) antigravityPlan = plan;
    char project[64] = {};
    if (antigravity::parseProject(response.c_str(), project, sizeof(project))) {
      antigravityProject = project;
    }
    antigravity::Credits credits;
    switch (antigravity::parseCredits(response.c_str(), credits)) {
      case antigravity::CreditsStatus::kFound:
        antigravityCredits = credits;
        antigravityCreditsOkMs = millis();
        return true;
      case antigravity::CreditsStatus::kAbsent:
        // 这台主机明确说没有 AI Credits；另一台可能还有，继续问。
        sawAbsent = true;
        continue;
      case antigravity::CreditsStatus::kInvalid:
        // 响应本身无效：换一台主机再试。
        continue;
    }
  }
  if (sawAbsent) {
    // 有合法响应明确没有这一项：清掉旧值，屏幕显示 `--`。
    antigravityCredits = antigravity::Credits{};
    antigravityCreditsOkMs = 0;
    return false;
  }
  // 这次没拿到有效响应（传输/HTTP 失败，或两台都返回坏数据）：保留旧值，
  // 但超过保鲜期就不再当作当前余额。
  if (antigravityCredits.present && antigravityCreditsOkMs != 0 &&
      millis() - antigravityCreditsOkMs > kAntigravityCreditsMaxAgeMs) {
    antigravityCredits = antigravity::Credits{};
    antigravityCreditsOkMs = 0;
  }
  return false;
}

// Antigravity OAuth 续期：refresh_token 换 access_token（Google 不轮换这个客户端的
// refresh token，但仍按同一套「先落 pending」保护处理）。
bool refreshAntigravityToken() {
  if (kClientId[0] == '\0' || kClientSecret[0] == '\0') {
    setError(UiState::kConfigError, "Antigravity 客户端未配置，无法续期，请重新运行 login");
    return false;
  }
  if (antigravityAuth.pending) {
    setError(UiState::kAuthError, "Antigravity 续期结果不确定，请重新运行 login");
    return false;
  }
  if (!resolveHost("oauth2.googleapis.com")) return false;
  WiFiClientSecure client;
  client.setCACertBundle(rootca_crt_bundle_start);
  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(30000);
  if (!http.begin(client, kAntigravityTokenUrl)) {
    setError(UiState::kTransportError, "Antigravity OAuth 请求初始化失败");
    return false;
  }
  http.addHeader("Accept", "application/json");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  const String body = "grant_type=refresh_token&client_id=" +
                      formEncode(kClientId) +
                      "&client_secret=" + formEncode(kClientSecret) +
                      "&refresh_token=" + formEncode(antigravityAuth.refreshToken);
  // 记录「尝试」时间，时钟未同步时用它限流。
  antigravityTokenRefreshMs = millis();
  AntigravityAuth pending = antigravityAuth;
  pending.pending = true;
  if (!saveAntigravityAuth(pending)) {
    http.end();
    setError(UiState::kConfigError, "无法保存 Antigravity 续期状态，未发送请求");
    return false;
  }
  antigravityAuth = pending;
  const int status = http.POST(body);
  const String response = status == 200 ? http.getString() : String();
  http.end();
  if (status <= 0) {
    // 请求可能已被上游处理：保留 pending，要求重新登录，避免重放旧令牌。
    setError(UiState::kAuthError, "Antigravity OAuth 续期响应丢失，请重新运行 login");
    return false;
  }
  if (status != 200) {
    const bool retryable = status == 429 || status >= 500;
    if (retryable) {
      pending.pending = false;
      if (saveAntigravityAuth(pending)) antigravityAuth = pending;
    }
    setError(antigravityAuth.pending ? UiState::kAuthError : classifyHttpStatus(status),
             "Antigravity OAuth 服务返回 HTTP " + String(status));
    return false;
  }
  cJSON* root = cJSON_ParseWithOpts(response.c_str(), nullptr, true);
  AntigravityAuth candidate = antigravityAuth;
  const cJSON* expires = cJSON_GetObjectItemCaseSensitive(root, "expires_in");
  const bool valid = jsonString(root, "access_token", candidate.accessToken) &&
                     !candidate.accessToken.isEmpty() &&
                     claude::integer(expires, 1, 365 * 86400);
  String rotated;
  if (valid && jsonString(root, "refresh_token", rotated) && !rotated.isEmpty()) {
    candidate.refreshToken = rotated;
  }
  if (valid) {
    candidate.expiresAt =
        claude::expiryFor(epochNow(), static_cast<uint32_t>(expires->valuedouble));
  }
  cJSON_Delete(root);
  if (!valid) {
    setError(UiState::kAuthError, "Antigravity OAuth 续期响应无效，请重新运行 login");
    return false;
  }
  candidate.pending = false;
  candidate.needsLogin = false;
  const AntigravityAuth previous = antigravityAuth;
  antigravityAuth = candidate;
  if (!saveAntigravityAuth(candidate)) {
    antigravityAuth = previous;
    setError(UiState::kAuthError, "Antigravity 新凭据写入失败，请重新运行 login");
    return false;
  }
  return true;
}

bool refreshAntigravityUsage() {
  if (antigravityAuth.pending || antigravityAuth.needsLogin) {
    setError(UiState::kAuthError, "Antigravity 凭据需要重新授权，请运行 login");
    return false;
  }
  bool refreshed = false;
  const uint32_t nowEpoch = epochNow();
  bool tokenDue = false;
  if (nowEpoch == 0) {
    tokenDue = antigravityTokenRefreshMs == 0 ||
               millis() - antigravityTokenRefreshMs >= kMinimumTokenRefreshMs;
  } else {
    tokenDue = antigravityAuth.expiresAt == 0 ||
               antigravityAuth.expiresAt <= static_cast<uint64_t>(nowEpoch) + 120;
  }
  if (tokenDue) {
    if (!refreshAntigravityToken()) return false;
    refreshed = true;
  }
  // 档位（含 project）与充值余额先抓：它们独立于配额，配额失败时也该显示出来。
  fetchAntigravityPlanAndCredits();
  RateWindow candidate;
  QuotaFetch result = fetchAntigravityQuota(candidate);
  if (result == QuotaFetch::kAuthRejected && !refreshed) {
    // 刚续期过就不再连环续期，避免时钟未同步或上游异常时反复打 token 端点。
    if (antigravityTokenRefreshMs != 0 &&
        millis() - antigravityTokenRefreshMs < 60 * 1000UL) {
      setError(UiState::kAuthError, "Antigravity 凭据验证失败，请稍后重试或重新 login");
      return false;
    }
    if (!refreshAntigravityToken()) return false;
    // 续期前那次档位/余额是用旧令牌抓的（很可能失败或没拿到 project）：
    // 用新令牌再抓一次，然后重试配额。
    fetchAntigravityPlanAndCredits();
    result = fetchAntigravityQuota(candidate);
  }
  if (result == QuotaFetch::kAuthRejected) {
    // 两个通道都拒绝：停止自动续期，等用户重新 login。
    setError(UiState::kAuthError, "Antigravity 凭据被拒绝，请重新运行 login");
    antigravityAuth.needsLogin = true;
    if (!saveAntigravityAuth(antigravityAuth)) {
      setError(UiState::kConfigError, "无法保存 Antigravity 需要重新登录的状态");
    }
    return false;
  }
  if (result == QuotaFetch::kBadData) {
    String detail = "Antigravity 配额响应里没有可用的 Gemini 每周额度";
    if (!debugAntigravityBuckets.isEmpty()) detail += "（" + debugAntigravityBuckets + "）";
    setError(UiState::kDataError, detail);
    return false;
  }
  if (result != QuotaFetch::kOk) {
    // 传输/HTTP 失败：postAntigravity 已经写了具体错误；这里只是兜底文案。
    if (lastErrorDetail.isEmpty()) {
      setError(UiState::kHttpError, "Antigravity 接口暂时不可用");
    }
    return false;
  }
  antigravitySeven = candidate;
  sourceSucceeded(antigravitySource, sourceInterval(quota::RowSource::kAntigravity));
  return true;
}

bool refreshBalance(const char* host, const char* url, const String& key,
                    bool (*parse)(const char*, balance_protocol::Balance&),
                    balance_protocol::Balance& balance) {
  if (!resolveHost(host)) return false;
  WiFiClientSecure client;
  client.setCACertBundle(rootca_crt_bundle_start);
  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(15000);
  if (!http.begin(client, url)) {
    setError(UiState::kTransportError, String(host) + " 请求初始化失败");
    return false;
  }
  http.addHeader("Accept", "application/json");
  http.addHeader("Authorization", "Bearer " + key);
  const int status = http.GET();
  const String body = http.getString();
  http.end();
  if (status <= 0) {
    setError(transportState(status, client), transportDetail(host, status, client));
    return false;
  }
  if (status != 200) {
    setError(classifyHttpStatus(status), String(host) + " HTTP " + String(status));
    return false;
  }
  if (!parse(body.c_str(), balance)) {
    setError(UiState::kDataError, String(host) + " 余额响应无效");
    return false;
  }
  return true;
}

// 同一平台的多个 key 逐个请求：单个 key 失败只标记它自己，其余 key 继续更新。
// 行状态取第一个失败 key 的错误，视图 2 显示它；视图 1 保留 * 标记该行有陈旧值。
void refreshBalanceGroup(const char* host, const char* url, const String& rawKeys,
                         uint32_t intervalSeconds,
                         bool (*parse)(const char*, balance_protocol::Balance&),
                         BalanceGroup& group) {
  key_list::List keys;
  if (!key_list::parse(rawKeys.c_str(), rawKeys.length(), keys)) {
    setError(UiState::kDataError, String(host) + " 余额 key 配置无效");
    group.count = 0;
    group.stale = true;
    sourceFailed(group.status, intervalSeconds, uiState, lastErrorDetail);
    return;
  }
  group.count = keys.count;
  group.stale = false;
  size_t failedCount = 0;
  UiState firstError = UiState::kReady;
  String firstDetail;
  for (size_t i = 0; i < keys.count; ++i) {
    const bool refreshed = refreshBalance(host, url, keys.items[i], parse, group.values[i]);
    group.slotStale[i] = !refreshed;
    group.stale = group.stale || !refreshed;
    if (refreshed) continue;
    if (failedCount == 0) {
      firstError = uiState;
      firstDetail = lastErrorDetail;
    }
    ++failedCount;
  }
  const quota::GroupDecision decision = quota::groupDecision(keys.count, failedCount);
  if (!decision.showError) {
    sourceSucceeded(group.status, intervalSeconds);
    return;
  }
  // 部分 key 失败时错的那个由视图 1 的 * 标记、视图 2 显示它的状态，
  // 但这一行仍按 50 秒节奏继续抓，健康 key 的数据不能被一个坏 key 拖旧。
  if (decision.keepsSchedule) {
    sourcePartiallySucceeded(group.status, intervalSeconds, firstError, firstDetail);
    return;
  }
  sourceFailed(group.status, intervalSeconds, firstError, firstDetail);
}

// 三个来源各自排队，只刷新到期的那些；手动刷新时全部一起尝试。
// 只有写进 rows 的来源才排队；不在屏幕上的来源完全不发请求。
struct DueSources {
  bool chatgpt;
  bool claude;
  bool antigravity;
  bool deepseek;
  bool openrouter;
};

DueSources dueSources(bool manual, uint32_t nowMs) {
  DueSources due{};
  due.chatgpt = rowEnabled(quota::RowSource::kChatgpt) && settings.codexConfigured() &&
                (manual || quota::refreshDue(nowMs, chatgptSource.dueAtMs));
  due.claude = rowEnabled(quota::RowSource::kClaude) && claudeAuth.configured() &&
               (manual || quota::refreshDue(nowMs, claudeSource.dueAtMs));
  due.antigravity = rowEnabled(quota::RowSource::kAntigravity) &&
                    antigravityAuth.configured() &&
                    (manual || quota::refreshDue(nowMs, antigravitySource.dueAtMs));
  due.deepseek = rowEnabled(quota::RowSource::kDeepseek) &&
                 settings.deepseekConfigured() &&
                 (manual || quota::refreshDue(nowMs, deepseekBalances.status.dueAtMs));
  due.openrouter = rowEnabled(quota::RowSource::kOpenrouter) &&
                   settings.openrouterConfigured() &&
                   (manual || quota::refreshDue(nowMs, openrouterBalances.status.dueAtMs));
  return due;
}

bool anyDue(const DueSources& due) {
  return due.chatgpt || due.claude || due.antigravity || due.deepseek || due.openrouter;
}

// 全局状态只服务于既有串口语义：按行顺序取第一个异常行，没有异常就是 kReady。
void syncOverallState() {
  uiState = UiState::kReady;
  lastErrorDetail = "";
  for (size_t i = 0; i < rowCount; ++i) {
    const SourceStatus* row = statusFor(rowConfigs[i].source);
    if (row != nullptr && quota::abnormal(row->state)) {
      uiState = row->state;
      lastErrorDetail = row->detail;
      return;
    }
  }
}

void refreshSources(const DueSources& due) {
  if (!anyDue(due)) return;
  // 函数返回时（含任何提前 return）统一收线；动画只认这一个标志。
  struct FetchClearer {
    ~FetchClearer() { fetchingMask = 0; }
  } clearOnExit;
  // 联网阶段先把本轮到期的行都点亮：WiFi 重连最长 15 秒，这段时间也该有进度线。
  // 联网就绪后改成按来源逐个点亮，避免「还在排队却显示在抓取」。
  fetchingMask = (due.chatgpt ? kFetchChatgpt : 0) | (due.claude ? kFetchClaude : 0) |
                 (due.antigravity ? kFetchAntigravity : 0) |
                 (due.deepseek ? kFetchDeepseek : 0) |
                 (due.openrouter ? kFetchOpenrouter : 0);
  if (!connectWiFi()) {
    fetchingMask = 0;
    // 联网失败时，本轮到期的每一行都显示同一个网络错误。
    if (due.chatgpt) {
      sourceFailed(chatgptSource, sourceInterval(quota::RowSource::kChatgpt), uiState,
                   lastErrorDetail);
    }
    if (due.claude) {
      sourceFailed(claudeSource, sourceInterval(quota::RowSource::kClaude), uiState,
                   lastErrorDetail);
    }
    if (due.antigravity) {
      sourceFailed(antigravitySource, sourceInterval(quota::RowSource::kAntigravity), uiState,
                   lastErrorDetail);
    }
    if (due.deepseek) {
      sourceFailed(deepseekBalances.status, sourceInterval(quota::RowSource::kDeepseek),
                   uiState, lastErrorDetail);
    }
    if (due.openrouter) {
      sourceFailed(openrouterBalances.status, sourceInterval(quota::RowSource::kOpenrouter),
                   uiState, lastErrorDetail);
    }
    return;
  }
  if (due.chatgpt) {
    fetchingMask = kFetchChatgpt;
    if (!refreshChatgptUsage()) {
      sourceFailed(chatgptSource, sourceInterval(quota::RowSource::kChatgpt), uiState,
                   lastErrorDetail);
    }
  }
  if (due.claude) {
    fetchingMask = kFetchClaude;
    if (!refreshClaudeUsage()) {
      sourceFailed(claudeSource, sourceInterval(quota::RowSource::kClaude), uiState,
                   lastErrorDetail);
    }
  }
  if (due.antigravity) {
    fetchingMask = kFetchAntigravity;
    if (!refreshAntigravityUsage()) {
      sourceFailed(antigravitySource, sourceInterval(quota::RowSource::kAntigravity), uiState,
                   lastErrorDetail);
    }
  }
  if (due.deepseek) {
    fetchingMask = kFetchDeepseek;
    refreshBalanceGroup("api.deepseek.com", "https://api.deepseek.com/user/balance",
                        settings.deepseekKeys, sourceInterval(quota::RowSource::kDeepseek),
                        balance_protocol::parseDeepSeek, deepseekBalances);
  }
  if (due.openrouter) {
    fetchingMask = kFetchOpenrouter;
    refreshBalanceGroup("openrouter.ai", "https://openrouter.ai/api/v1/credits",
                        settings.openrouterKeys, sourceInterval(quota::RowSource::kOpenrouter),
                        balance_protocol::parseOpenRouter, openrouterBalances);
  }
}

uint16_t nextCodepoint(const char*& cursor) {
  const uint8_t first = static_cast<uint8_t>(*cursor++);
  if (first < 0x80) return first;
  if ((first & 0xe0) == 0xc0) {
    const uint8_t second = static_cast<uint8_t>(*cursor++);
    return static_cast<uint16_t>(((first & 0x1f) << 6) | (second & 0x3f));
  }
  const uint8_t second = static_cast<uint8_t>(*cursor++);
  const uint8_t third = static_cast<uint8_t>(*cursor++);
  return static_cast<uint16_t>(((first & 0x0f) << 12) |
                               ((second & 0x3f) << 6) | (third & 0x3f));
}

int16_t uiTextWidth(const char* text) {
  int16_t width = 0;
  while (*text != '\0') {
    const uint16_t codepoint = nextCodepoint(text);
    width += codepoint < 0x80 ? 6 : ui_font::kGlyphWidth;
  }
  return width;
}

void drawUiText(int16_t x, int16_t top, const char* text) {
  while (*text != '\0') {
    const uint16_t codepoint = nextCodepoint(text);
    if (codepoint < 0x80) {
      display.drawGlyph(x, top + 11, codepoint);
      x += 6;
      continue;
    }
    const uint8_t* bitmap = ui_font::find(codepoint);
    if (bitmap != nullptr) {
      display.drawXBMP(x, top, ui_font::kGlyphWidth,
                       ui_font::kGlyphHeight, bitmap);
    }
    x += ui_font::kGlyphWidth;
  }
}

// 窗口行需要的数据：badge 由来源决定；creditMode 表示额度已用尽（或充值余额不足），
// 这时右侧改显示充值余额而不是配速。
struct WindowRowData {
  const char* badge = nullptr;
  bool creditMode = false;
  bool creditsKnown = false;
  double creditAmount = 0;
};

// 绘制本身放在 window_render.h：固件和电脑端的视觉验证脚本用同一份代码。
void drawWindowLine(int16_t y, const RateWindow& window, const WindowRowData& data) {
  window_render::Input input;
  input.present = window.present;
  input.resetAt = window.resetAt;
  input.durationSeconds = window.durationSeconds;
  input.usedPercent = window.usedPercent;
  input.nowEpoch = epochNow();
  input.badge = data.badge;
  input.creditMode = data.creditMode;
  input.creditsKnown = data.creditsKnown;
  input.creditAmount = data.creditAmount;
  window_render::drawWindowRow(display.getU8g2(), y, input, u8g2_font_7x14_tf);
}

// 视图 2：左侧平台名（可带 * 陈旧标记）与过期时间，右侧状态词。
// 排版由 quota::statusLineLayout 决定，放不下时先丢平台名再丢 *，状态词始终完整。
void drawStatusLine(int16_t top, const char* label, bool stale, const char* age,
                    const char* state) {
  display.setFont(u8g2_font_6x12_tf);  // ASCII 走 6×12，与中文 12×12 同高
  const quota::StatusLineLayout layout = quota::statusLineLayout(
      128, uiTextWidth(state), label[0] != '\0' ? uiTextWidth(label) : 0,
      uiTextWidth(age), stale);
  if (layout.showLabel) drawUiText(layout.labelX, top, label);
  if (layout.showStar) drawUiText(layout.starX, top, "*");
  if (layout.showAge) drawUiText(layout.ageX, top, age);
  if (state[0] != '\0') drawUiText(layout.stateX, top, state);
}

// 视图 2 的整行：平台名（可带 *）、过期时间、状态词。age 由调用方给栈上缓冲区。
void drawStatusRow(int16_t y, const char* label, bool stale, uint32_t lastOkMs,
                   uint32_t intervalSeconds, UiState state) {
  char age[12];
  if (!quota::ageSince(lastOkMs, millis(), intervalSeconds, age, sizeof(age))) {
    std::snprintf(age, sizeof(age), "--");
  }
  drawStatusLine(y - 11, label, stale, age, uiMessage(state));
}

// 多个余额用 + 拼接，+ 左右各留 1px，避免与两侧数字贴在一起。
constexpr int16_t kJoinPadding = 1;

int16_t joinedAmountWidth(const char* text) {
  int16_t width = display.getStrWidth(text);
  for (const char* cursor = text; *cursor != '\0'; ++cursor) {
    if (*cursor == '+') width += 2 * kJoinPadding;
  }
  return width;
}

void drawJoinedAmount(int16_t x, int16_t y, const char* text) {
  char segment[32];
  size_t length = 0;
  for (const char* cursor = text;; ++cursor) {
    if (*cursor != '\0' && *cursor != '+') {
      if (length + 1 < sizeof(segment)) segment[length++] = *cursor;
      continue;
    }
    if (length > 0) {
      segment[length] = '\0';
      display.drawStr(x, y, segment);
      x += display.getStrWidth(segment);
      length = 0;
    }
    if (*cursor == '\0') return;
    x += kJoinPadding;
    display.drawStr(x, y, "+");
    x += display.getStrWidth("+") + kJoinPadding;
  }
}

void drawBalanceLine(int16_t y, const char* label, const BalanceGroup& group) {
  char amount[96];
  if (!balance_format::joined(group.values, group.count, amount, sizeof(amount))) {
    std::snprintf(amount, sizeof(amount), "--");
  }
  const bool stale = group.stale && group.anyPresent();
  struct Layout {
    const uint8_t* amountFont;
    const uint8_t* labelFont;
    bool showLabel;
  };
  // 单个余额沿用原来的 6×12 平台名；显示 2 个及以上余额时改用更小的 5×8
  // 平台名，把宽度让给 + 拼接的金额，金额仍优先保持 7×14。key 更多时继续
  // 逐级缩小，极端情况下省略平台名。
  const uint8_t* primaryLabelFont =
      group.count >= 2 ? u8g2_font_5x8_tf : u8g2_font_6x12_tf;
  const Layout layouts[] = {
      {u8g2_font_7x14_tf, primaryLabelFont, true},
      {u8g2_font_6x12_tf, primaryLabelFont, true},
      {u8g2_font_5x8_tf, u8g2_font_5x8_tf, true},
      {u8g2_font_6x12_tf, u8g2_font_6x12_tf, false},
      {u8g2_font_5x8_tf, u8g2_font_5x8_tf, false},
  };
  const Layout* chosen = &layouts[sizeof(layouts) / sizeof(layouts[0]) - 1];
  int16_t labelWidth = 0;
  int16_t amountWidth = 0;
  for (const Layout& layout : layouts) {
    display.setFont(layout.labelFont);
    labelWidth = layout.showLabel ? display.getStrWidth(label) : 0;
    const int16_t markerWidth = stale ? display.getStrWidth("*") : 0;
    display.setFont(layout.amountFont);
    amountWidth = joinedAmountWidth(amount);
    const int16_t marker = layout.showLabel ? labelWidth + markerWidth + 1 : markerWidth + 2;
    const int16_t needed = marker + (layout.showLabel ? 2 : 0) + amountWidth;
    if (needed <= 128) {
      chosen = &layout;
      break;
    }
  }
  display.setFont(chosen->labelFont);
  if (chosen->showLabel) {
    display.drawStr(0, y, label);
    labelWidth = display.getStrWidth(label);
    if (stale) display.drawStr(labelWidth + 1, y, "*");
  } else if (stale) {
    display.drawStr(0, y, "*");
  }
  display.setFont(chosen->amountFont);
  amountWidth = joinedAmountWidth(amount);
  const int16_t x = amountWidth < 128 ? 128 - amountWidth : 0;
  drawJoinedAmount(x, y, amount);
}

// 屏幕与行的对应关系不写死：行数、顺序、窗口来自 NVS 的 rows 配置，
// 位置来自 quota::rowLayout()。绘制、动画和调度共用同一份映射。
constexpr int16_t kDisplayWidth = 128;

struct RowView {
  quota::RowConfig config;
  quota::RowSlotLayout slot;
  SourceStatus* status;
  const RateWindow* window;
  const BalanceGroup* balances;
  const char* label;
  uint8_t fetchBit;
};

uint8_t fetchBitFor(quota::RowSource source) {
  switch (source) {
    case quota::RowSource::kChatgpt: return kFetchChatgpt;
    case quota::RowSource::kClaude: return kFetchClaude;
    case quota::RowSource::kAntigravity: return kFetchAntigravity;
    case quota::RowSource::kDeepseek: return kFetchDeepseek;
    case quota::RowSource::kOpenrouter: return kFetchOpenrouter;
  }
  return 0;
}

// 按当前配置生成行视图；rowCount 为 0（还没加载配置）时返回 0，屏幕上不画东西。
size_t buildRows(RowView* rows) {
  quota::RowSlotLayout layout[quota::kMaxRows];
  if (!quota::rowLayout(rowCount, layout)) return 0;
  for (size_t i = 0; i < rowCount; ++i) {
    RowView& row = rows[i];
    row.config = rowConfigs[i];
    row.slot = layout[i];
    row.status = statusFor(rowConfigs[i].source);
    row.label = quota::rowSourceName(rowConfigs[i].source);
    row.fetchBit = fetchBitFor(rowConfigs[i].source);
    row.window = nullptr;
    row.balances = nullptr;
    switch (rowConfigs[i].source) {
      case quota::RowSource::kChatgpt:
        row.window = rowConfigs[i].window == quota::WindowKind::kFiveHour ? &chatgptFive
                                                                          : &chatgptSeven;
        break;
      case quota::RowSource::kClaude:
        row.window = rowConfigs[i].window == quota::WindowKind::kFiveHour ? &fiveHour
                                                                          : &sevenDay;
        break;
      case quota::RowSource::kAntigravity:
        // 只取每周额度：antigravity 行固定 7d（配置层已拒绝 :5h）。
        row.window = &antigravitySeven;
        break;
      case quota::RowSource::kDeepseek:
        row.balances = &deepseekBalances;
        break;
      case quota::RowSource::kOpenrouter:
        row.balances = &openrouterBalances;
        break;
    }
  }
  return rowCount;
}

bool rowHasData(const RowView& row) {
  return row.window != nullptr ? row.window->present
                               : (row.balances != nullptr && row.balances->anyPresent());
}

// 窗口行没有多 key 概念，不存在「旧值」标记；余额行才可能标 *。
bool rowStale(const RowView& row) {
  return row.balances != nullptr && row.balances->stale && row.balances->anyPresent();
}

void drawRow(const RowView& row, uint8_t phase) {
  if (quota::showStatusView(quota::abnormal(row.status->state), rowHasData(row), phase)) {
    // 新鲜度按「该来源实际生效的间隔」判定：同一来源占多行时抓取取最短的
    // 间隔，展示也用同一个值，避免 5h/7d 两行对同一份数据给出不同新旧。
    drawStatusRow(row.slot.textY, row.label, rowStale(row), row.status->lastOkMs,
                  sourceInterval(row.config.source), row.status->state);
    return;
  }
  if (row.window != nullptr) {
    WindowRowData data;
    data.badge = quota::rowBadgeLetter(row.config.source);
    if (row.config.source == quota::RowSource::kAntigravity) {
      // 只有额度用尽（屏幕 0%）时才把右侧改成充值余额；余额低于 minimum 不切换。
      data.creditMode = antigravity::overageMode(*row.window, antigravityCredits);
      data.creditsKnown = antigravityCredits.present;
      data.creditAmount = antigravityCredits.amount;
    }
    drawWindowLine(row.slot.textY, *row.window, data);
  } else {
    drawBalanceLine(row.slot.textY, row.label, *row.balances);
  }
}

// 没有任何异常行时两个视图完全相同，不需要为了轮换重画。
bool anyRowAbnormal() {
  for (size_t i = 0; i < rowCount; ++i) {
    const SourceStatus* status = statusFor(rowConfigs[i].source);
    if (status != nullptr && quota::abnormal(status->state)) return true;
  }
  return false;
}

// 先把行内的 1px 线清空，再给 mask 里正在抓取的行画上高光；屏幕外的部分由 u8g2 裁掉。
// 只由动画任务调用：整屏重画不再画线，避免「全屏画过、任务却不知道」的残影。
void drawLoadingLines(const RowView* rows, size_t count, uint32_t nowMs, uint8_t mask) {
  const quota::LoadingBarSpan span = quota::loadingBarSpan(nowMs, kDisplayWidth);
  display.setDrawColor(0);
  for (size_t i = 0; i < count; ++i) {
    display.drawBox(0, rows[i].slot.lineY, kDisplayWidth, 1);
  }
  display.setDrawColor(1);
  for (size_t i = 0; i < count; ++i) {
    if ((mask & rows[i].fetchBit) != 0) {
      display.drawBox(span.x, rows[i].slot.lineY, span.width, 1);
    }
  }
}

// 整屏重画后线会被一起刷掉，动画任务下一帧必须整段补画一次（而不是只补两端）。
// 写端和读端都在 displayMutex 里，所以不需要额外同步。
bool loadingBarNeedsFullFlush = true;

void drawScreen() {
  if (displayMutex != nullptr) xSemaphoreTake(displayMutex, portMAX_DELAY);
  RowView rows[quota::kMaxRows];
  const size_t count = buildRows(rows);
  const uint8_t phase = quota::viewPhase(millis());
  display.clearBuffer();
  for (size_t i = 0; i < count; ++i) drawRow(rows[i], phase);
  display.sendBuffer();
  loadingBarNeedsFullFlush = true;
  drawnPhase = phase;
  if (displayMutex != nullptr) xSemaphoreGive(displayMutex);
}

// 后台动画：请求是阻塞的，只有在独立任务里刷新这 1px 才会在等待期间继续移动。
// 每帧只重发高光扫过的 tile 列，不动整屏。
void loadingAnimationTask(void*) {
  uint8_t paintedMask = 0;  // 屏幕上当前画着线的行；刚停止的行还要再刷一次擦掉
  int16_t previousX = 0;
  for (;;) {
    const uint32_t now = millis();
    // 先无锁看一眼要不要画：空闲时绝大多数轮次直接跳到延时，不去抢显示锁。
    if (fetchingMask != 0 || paintedMask != 0) {
      if (displayMutex != nullptr && xSemaphoreTake(displayMutex, pdMS_TO_TICKS(40)) == pdTRUE) {
        const uint8_t mask = fetchingMask;  // 锁内再读一次，避免判断和绘制看到不同状态
        RowView rows[quota::kMaxRows];
        const size_t count = buildRows(rows);
        drawLoadingLines(rows, count, now, mask);
        const quota::LoadingBarSpan span = quota::loadingBarSpan(now, kDisplayWidth);
        const uint8_t dirty = static_cast<uint8_t>(mask | paintedMask);
        // 整段重画只在「刚起步 / 行数变了 / 整屏刚重画过」时用；其余帧只补两端。
        // 位集合有任何变化（包括「从多行切到少行」）都要整段补画：缓冲里的线
        // 已整体清空，只补两端会留下被移除那几行的残余高光。
        const bool fullFlush = loadingBarNeedsFullFlush || mask != paintedMask;
        const quota::LoadingBarFlush flush =
            fullFlush ? quota::loadingBarFlush(previousX, span.x, quota::kLoadingBarWidth,
                                               kDisplayWidth)
                      : quota::loadingBarFlushDiff(previousX, span.x, quota::kLoadingBarWidth,
                                                   kDisplayWidth);
        for (uint8_t s = 0; s < flush.count; ++s) {
          for (size_t i = 0; i < count; ++i) {
            if ((dirty & rows[i].fetchBit) == 0) continue;
            display.updateDisplayArea(flush.spans[s].tx,
                                      static_cast<uint8_t>(rows[i].slot.lineY / 8),
                                      flush.spans[s].tw, 1);
          }
        }
        paintedMask = mask;
        previousX = span.x;
        loadingBarNeedsFullFlush = false;
        xSemaphoreGive(displayMutex);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(kLoadingFrameMs));
  }
}

void sendResponse(cJSON* response) {
  char* encoded = cJSON_PrintUnformatted(response);
  if (encoded != nullptr) {
    Serial.println(encoded);
    cJSON_free(encoded);
  }
}

void addResponseBase(cJSON* response, const String& requestId, bool ok) {
  cJSON_AddStringToObject(response, "request_id", requestId.c_str());
  cJSON_AddBoolToObject(response, "ok", ok);
}

void handleSerialCommand(const String& line) {
  cJSON* root = cJSON_ParseWithLength(line.c_str(), line.length());
  if (root == nullptr) return;
  String requestId;
  String command;
  jsonString(root, "request_id", requestId);
  jsonString(root, "cmd", command);
  // USB retries can arrive after a refresh has rotated credentials. A repeated
  // successful mutation acknowledges the original request without replaying it.
  if (!requestId.isEmpty() && requestId == lastMutationId && command == lastMutationCommand) {
    Serial.println(lastMutationReply);
    cJSON_Delete(root);
    return;
  }
  cJSON* response = cJSON_CreateObject();

  if (requestId.isEmpty() || command.isEmpty()) {
    addResponseBase(response, requestId, false);
    cJSON_AddStringToObject(response, "error", "命令格式错误");
  } else if (command == "configure") {
    Settings candidate = settings;
    cJSON* utcOffset = cJSON_GetObjectItemCaseSensitive(root, "utc_offset_minutes");
    cJSON* expiresAt = cJSON_GetObjectItemCaseSensitive(root, "expires_at");
    const bool authFieldsProvided =
        cJSON_GetObjectItemCaseSensitive(root, "access_token") != nullptr ||
        cJSON_GetObjectItemCaseSensitive(root, "refresh_token") != nullptr ||
        cJSON_GetObjectItemCaseSensitive(root, "account_id") != nullptr ||
        expiresAt != nullptr;
    const bool deepseekProvided =
        cJSON_GetObjectItemCaseSensitive(root, "deepseek_key") != nullptr;
    const bool openrouterProvided =
        cJSON_GetObjectItemCaseSensitive(root, "openrouter_key") != nullptr;
    const bool rowsProvided = cJSON_GetObjectItemCaseSensitive(root, "rows") != nullptr;
    // 同一请求的重发（USB 重试/重启后重放）不得重新安装已经处理过的凭据：
    // 期间设备可能已经续期并轮换掉旧 refresh token。
    const bool authAlreadyApplied = authFieldsProvided && !requestId.isEmpty() &&
                                    requestId == settings.codexProvisionId;
    quota::RowConfig parsedRows[quota::kMaxRows];
    size_t parsedRowCount = 0;
    const bool keyFieldsProvided = deepseekProvided || openrouterProvided;
    const bool ssidProvided =
        cJSON_GetObjectItemCaseSensitive(root, "wifi_ssid") != nullptr;
    const bool passwordProvided =
        cJSON_GetObjectItemCaseSensitive(root, "wifi_password") != nullptr;

    // 部分更新：只校验并套用请求里出现的字段，其余保留设备原值。
    bool valid = ssidProvided || passwordProvided || utcOffset != nullptr ||
                 authFieldsProvided || keyFieldsProvided || rowsProvided;
    if (ssidProvided) {
      valid = valid && jsonString(root, "wifi_ssid", candidate.wifiSsid) &&
              !candidate.wifiSsid.isEmpty();
    }
    if (passwordProvided) {
      valid = valid && jsonString(root, "wifi_password", candidate.wifiPassword);
    }
    if (utcOffset != nullptr) {
      valid = valid && cJSON_IsNumber(utcOffset) &&
              utcOffset->valuedouble >= -840 && utcOffset->valuedouble <= 840;
    }
    if (authFieldsProvided && !authAlreadyApplied) {
      valid = valid &&
              jsonString(root, "access_token", candidate.accessToken) &&
              jsonString(root, "refresh_token", candidate.refreshToken) &&
              jsonString(root, "account_id", candidate.accountId) &&
              cJSON_IsNumber(expiresAt) && expiresAt->valuedouble > 0 &&
              !candidate.accessToken.isEmpty() && !candidate.refreshToken.isEmpty() &&
              !candidate.accountId.isEmpty();
    }
    if (rowsProvided) {
      // 行配置在设备侧再校验一次：空列表、超过 3 行、未知来源或非法窗口都拒绝。
      valid = valid && jsonString(root, "rows", candidate.rows) &&
              quota::parseRows(candidate.rows.c_str(), candidate.rows.length(), parsedRows,
                               parsedRowCount);
    }
    if (keyFieldsProvided) {
      // 两个平台可以独立更新；每个平台的 key 列表最多 key_list::kMaxKeys 个。
      key_list::List keys;
      if (deepseekProvided) {
        valid = valid &&
                jsonString(root, "deepseek_key", candidate.deepseekKeys) &&
                key_list::parse(candidate.deepseekKeys.c_str(),
                                candidate.deepseekKeys.length(), keys) &&
                keys.count > 0;
      }
      if (openrouterProvided) {
        valid = valid &&
                jsonString(root, "openrouter_key", candidate.openrouterKeys) &&
                key_list::parse(candidate.openrouterKeys.c_str(),
                                candidate.openrouterKeys.length(), keys) &&
                keys.count > 0;
      }
    }
    if (valid) {
      if (utcOffset != nullptr) {
        candidate.utcOffsetMinutes = static_cast<int16_t>(utcOffset->valuedouble);
      }
      if (authFieldsProvided && !authAlreadyApplied) {
        candidate.expiresAt = static_cast<uint64_t>(expiresAt->valuedouble);
        // 新凭据必须解除上一轮的 pending / needs-login，否则永远不会再自动抓取。
        candidate.codexPending = false;
        candidate.codexNeedsLogin = false;
        // 记下这次安装请求，重发时只确认、不重装。
        candidate.codexProvisionId = requestId;
      }
      const Settings previous = settings;
      settings = candidate;
      if (saveSettings()) {
        addResponseBase(response, requestId, true);
        if (rowsProvided) {
          applyRowConfig(parsedRows, parsedRowCount);
          resetDisabledSourceStatus();
          // 新加进来的来源立即排一次；被移除的来源不再排队（dueAtMs 不再被检查）。
          chatgptSource.dueAtMs = 0;
          claudeSource.dueAtMs = 0;
          antigravitySource.dueAtMs = 0;
          deepseekBalances.status.dueAtMs = 0;
          openrouterBalances.status.dueAtMs = 0;
        }
        if (authFieldsProvided && !authAlreadyApplied) {
          // 换账号后上一份用量不再属于当前凭据，先清掉再重画。
          chatgptFive = RateWindow{};
          chatgptSeven = RateWindow{};
          chatgptSource = SourceStatus{};
          codexTokenRefreshMs = millis();
        }
        if (deepseekProvided) {
          resetBalanceGroup(deepseekBalances, settings.deepseekKeys);
        }
        if (openrouterProvided) {
          resetBalanceGroup(openrouterBalances, settings.openrouterKeys);
        }
        WiFi.disconnect(true, false);
        refreshRequested = true;
        uiState = settings.configured() ? UiState::kLoading : UiState::kUnconfigured;
        lastErrorDetail = "";
        // 配置可能改变了行布局，也可能清空了数据：立即按最终状态重画，不能等
        // 一次可能很慢甚至不会发生的抓取。
        drawScreen();
      } else {
        settings = previous;
        addResponseBase(response, requestId, false);
        cJSON_AddStringToObject(response, "error", "配置写入失败");
      }
    } else {
      addResponseBase(response, requestId, false);
      cJSON_AddStringToObject(response, "error", "配置字段无效");
    }
  } else if (command == "claude_login" && requestId == claudeAuth.provisionId) {
    // Persisted identity also protects against a USB retry after a reboot.
    addResponseBase(response, requestId, true);
    cJSON_AddStringToObject(response, "provider", "claude");
  } else if (command == "claude_login") {
    ClaudeAuth candidate;
    candidate.provisionId = requestId;
    cJSON* expires = cJSON_GetObjectItemCaseSensitive(root, "expires_at");
    const bool valid = jsonString(root, "access_token", candidate.accessToken) &&
        jsonString(root, "refresh_token", candidate.refreshToken) && candidate.configured() &&
        claude::integer(expires, kMinimumValidEpoch, UINT32_MAX);
    if (valid) candidate.expiresAt = static_cast<uint64_t>(expires->valuedouble);
    if (valid && saveClaudeAuth(candidate)) {
      claudeAuth = candidate;
      fiveHour = RateWindow{};
      sevenDay = RateWindow{};
      // 新账号：清掉旧错误/旧 age 和旧账号的 429 冷却，避免拖住首次验证。
      claudeSource = SourceStatus{};
      claudeBlockedUntil = 0;
      preferences.remove("claude_wait");
      claudeTokenRefreshMs = millis();
      lastRefreshEpoch = 0;
      refreshRequested = true;
      lastErrorDetail = "";
      uiState = settings.configured() ? UiState::kLoading : UiState::kUnconfigured;
      drawScreen();
      addResponseBase(response, requestId, true);
      cJSON_AddStringToObject(response, "provider", "claude");
    } else {
      addResponseBase(response, requestId, false);
      cJSON_AddStringToObject(response, "error", valid ? "Claude 凭据写入失败" : "Claude 登录字段无效");
    }
  } else if (command == "antigravity_login" && requestId == antigravityAuth.provisionId) {
    // 同一登录请求的重发（USB 重试/重启后重放）：只确认，不重新安装旧 token。
    addResponseBase(response, requestId, true);
    cJSON_AddStringToObject(response, "provider", "antigravity");
  } else if (command == "antigravity_login") {
    AntigravityAuth candidate;
    candidate.provisionId = requestId;
    cJSON* expires = cJSON_GetObjectItemCaseSensitive(root, "expires_at");
    // 与 loadAntigravityAuth 同一口径：写入的时间必须能在下次启动读回，
    // 否则会出现「登录成功但重启后凭据丢失」。
    const bool valid = jsonString(root, "access_token", candidate.accessToken) &&
        jsonString(root, "refresh_token", candidate.refreshToken) &&
        candidate.configured() &&
        claude::integer(expires, kMinimumValidEpoch, UINT32_MAX);
    if (valid) {
      candidate.expiresAt = static_cast<uint64_t>(expires->valuedouble);
      jsonString(root, "account", candidate.account);
    }
    if (valid && saveAntigravityAuth(candidate)) {
      antigravityAuth = candidate;
      // 换账号后上一份额度/余额不再属于当前凭据，先清掉再重画。
      antigravitySeven = RateWindow{};
      antigravityCredits = antigravity::Credits{};
      antigravityCreditsOkMs = 0;
      antigravityPlan = "";
      antigravityProject = "";
      antigravitySource = SourceStatus{};
      antigravityTokenRefreshMs = millis();
      lastRefreshEpoch = 0;
      refreshRequested = true;
      lastErrorDetail = "";
      uiState = settings.configured() ? UiState::kLoading : UiState::kUnconfigured;
      drawScreen();
      addResponseBase(response, requestId, true);
      cJSON_AddStringToObject(response, "provider", "antigravity");
    } else {
      addResponseBase(response, requestId, false);
      cJSON_AddStringToObject(response, "error",
                              valid ? "Antigravity 凭据写入失败" : "Antigravity 登录字段无效");
    }
  } else if (command == "login" && !requestId.isEmpty() &&
             requestId == settings.codexProvisionId) {
    // 同一登录请求的重发（USB 重试/重启后重放）：只确认，不重新安装旧 token。
    addResponseBase(response, requestId, true);
    cJSON_AddStringToObject(response, "account_id", settings.accountId.c_str());
  } else if (command == "login") {
    Settings candidate = settings;
    cJSON* expiresAt = cJSON_GetObjectItemCaseSensitive(root, "expires_at");
    const bool valid =
        jsonString(root, "access_token", candidate.accessToken) &&
        jsonString(root, "refresh_token", candidate.refreshToken) &&
        jsonString(root, "account_id", candidate.accountId) &&
        cJSON_IsNumber(expiresAt) && expiresAt->valuedouble > 0 &&
        !candidate.accessToken.isEmpty() && !candidate.refreshToken.isEmpty() &&
        !candidate.accountId.isEmpty();
    if (valid) {
      candidate.expiresAt = static_cast<uint64_t>(expiresAt->valuedouble);
      candidate.codexPending = false;
      candidate.codexNeedsLogin = false;
      candidate.codexProvisionId = requestId;
      const Settings previous = settings;
      settings = candidate;
      if (saveCodexAuth()) {
        addResponseBase(response, requestId, true);
        cJSON_AddStringToObject(response, "account_id", settings.accountId.c_str());
        WiFi.disconnect(true, false);
        refreshRequested = true;
        // 新账号：清掉上一账号的用量快照与失败状态，首次成功抓取前不显示旧数据。
        chatgptFive = RateWindow{};
        chatgptSeven = RateWindow{};
        chatgptSource = SourceStatus{};
        codexTokenRefreshMs = millis();
        uiState = settings.configured() ? UiState::kLoading : UiState::kUnconfigured;
        lastErrorDetail = "";
        drawScreen();
      } else {
        settings = previous;
        addResponseBase(response, requestId, false);
        cJSON_AddStringToObject(response, "error", "登录凭据写入失败");
      }
    } else {
      addResponseBase(response, requestId, false);
      cJSON_AddStringToObject(response, "error", "登录字段无效");
    }
  } else if (command == "status") {
    addResponseBase(response, requestId, true);
    cJSON_AddStringToObject(response, "usage_provider", "claude");
    cJSON_AddBoolToObject(response, "claude_supported", true);
    cJSON_AddBoolToObject(response, "claude_configured", claudeAuth.configured());
    cJSON_AddBoolToObject(response, "claude_needs_login", claudeAuth.pending);
    cJSON_AddNumberToObject(response, "claude_expires_at", static_cast<double>(claudeAuth.expiresAt));
    cJSON_AddNumberToObject(response, "claude_retry_at", claudeBlockedUntil);
    auto addWindow = [&](const char* name, const RateWindow& window) {
      cJSON* value = cJSON_AddObjectToObject(response, name);
      cJSON_AddBoolToObject(value, "present", window.present);
      if (window.present) {
        cJSON_AddNumberToObject(value, "used_percent", window.usedPercent);
        cJSON_AddNumberToObject(value, "reset_at", window.resetAt);
        cJSON_AddBoolToObject(value, "expired", window.resetAt && epochNow() >= window.resetAt);
      }
    };
    addWindow("five_hour", fiveHour);
    addWindow("seven_day", sevenDay);
    addWindow("chatgpt_five_hour", chatgptFive);
    addWindow("chatgpt_seven_day", chatgptSeven);
    addWindow("antigravity_seven_day", antigravitySeven);
    auto addBalance = [&](const char* summaryName, const char* listName,
                          const BalanceGroup& group) {
      bool present = false;
      bool stale = false;
      double total = 0.0;
      const char* currency = nullptr;
      for (size_t i = 0; i < group.count; ++i) {
        if (group.values[i].present) {
          present = true;
          total += group.values[i].remaining;
          if (currency == nullptr) currency = group.values[i].currency;
        }
        stale = stale || group.slotStale[i];
      }
      // 汇总字段保留原有形状：remaining 为已取得余额的合计，count 为 key 数。
      cJSON* summary = cJSON_AddObjectToObject(response, summaryName);
      cJSON_AddBoolToObject(summary, "present", present);
      cJSON_AddBoolToObject(summary, "stale", stale);
      cJSON_AddNumberToObject(summary, "count", static_cast<double>(group.count));
      if (present) {
        cJSON_AddNumberToObject(summary, "remaining", total);
        cJSON_AddStringToObject(summary, "currency", currency);
      }
      cJSON* entries = cJSON_AddArrayToObject(response, listName);
      for (size_t i = 0; i < group.count; ++i) {
        cJSON* entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "index", static_cast<double>(i));
        cJSON_AddBoolToObject(entry, "present", group.values[i].present);
        cJSON_AddBoolToObject(entry, "stale", group.slotStale[i]);
        if (group.values[i].present) {
          cJSON_AddNumberToObject(entry, "remaining", group.values[i].remaining);
          cJSON_AddStringToObject(entry, "currency", group.values[i].currency);
        }
        cJSON_AddItemToArray(entries, entry);
      }
    };
    addBalance("deepseek_balance", "deepseek_balances", deepseekBalances);
    addBalance("openrouter_balance", "openrouter_balances", openrouterBalances);
    auto addSourceStatus = [&](const char* name, const SourceStatus& source) {
      cJSON* row = cJSON_AddObjectToObject(response, name);
      cJSON_AddStringToObject(row, "state", uiMessage(source.state));
      cJSON_AddStringToObject(row, "error_detail", source.detail.c_str());
      // -1 表示这一行还没有过成功记录；0 才是“刚刚成功过”，两者不能混。
      cJSON_AddNumberToObject(row, "age_seconds",
                              source.lastOkMs == 0 ? -1
                                  : static_cast<double>((millis() - source.lastOkMs) / 1000UL));
      const int32_t dueMs = static_cast<int32_t>(source.dueAtMs - millis());
      cJSON_AddNumberToObject(row, "next_refresh_seconds",
                              source.dueAtMs != 0 && dueMs > 0 ? dueMs / 1000 : 0);
    };
    addSourceStatus("chatgpt_status", chatgptSource);
    addSourceStatus("claude_status", claudeSource);
    addSourceStatus("antigravity_status", antigravitySource);
    addSourceStatus("deepseek_status", deepseekBalances.status);
    addSourceStatus("openrouter_status", openrouterBalances.status);
    cJSON_AddBoolToObject(response, "configured", settings.configured());
    char rowsText[96] = {};
    if (quota::formatRows(rowConfigs, rowCount, rowsText, sizeof(rowsText))) {
      cJSON_AddStringToObject(response, "rows", rowsText);
    } else {
      cJSON_AddStringToObject(response, "rows", "");
    }
    cJSON_AddBoolToObject(response, "chatgpt_configured", settings.codexConfigured());
    cJSON_AddBoolToObject(response, "chatgpt_needs_login",
                          settings.codexNeedsLogin || settings.codexPending);
    cJSON_AddBoolToObject(response, "chatgpt_pending", settings.codexPending);
    cJSON_AddBoolToObject(response, "codex_configured", settings.codexConfigured());
    cJSON_AddBoolToObject(response, "antigravity_configured", antigravityAuth.configured());
    cJSON_AddBoolToObject(response, "antigravity_needs_login",
                          antigravityAuth.needsLogin || antigravityAuth.pending);
    cJSON_AddBoolToObject(response, "antigravity_pending", antigravityAuth.pending);
    if (!antigravityAuth.account.isEmpty()) {
      cJSON_AddStringToObject(response, "antigravity_account",
                              antigravityAuth.account.c_str());
    }
    if (!antigravityPlan.isEmpty()) {
      cJSON_AddStringToObject(response, "antigravity_plan", antigravityPlan.c_str());
    }
    // 充值余额：present=false 表示这次（或至今没有一次）响应里没带 availableCredits。
    // amount 已按 100 credits = 1 美元换算；minimum 低于它就不能再用于计费。
    cJSON* credits = cJSON_AddObjectToObject(response, "antigravity_credits");
    cJSON_AddBoolToObject(credits, "present", antigravityCredits.present);
    if (antigravityCredits.present) {
      cJSON_AddNumberToObject(credits, "amount", antigravityCredits.amount);
      cJSON_AddNumberToObject(credits, "minimum", antigravityCredits.minimum);
    }
    const bool antigravityOverage = antigravity::overageMode(antigravitySeven, antigravityCredits);
    cJSON_AddBoolToObject(response, "antigravity_overage", antigravityOverage);
    cJSON_AddBoolToObject(response, "deepseek_configured", settings.deepseekConfigured());
    cJSON_AddBoolToObject(response, "openrouter_configured", settings.openrouterConfigured());
    cJSON_AddBoolToObject(response, "wifi_connected", WiFi.status() == WL_CONNECTED);
    cJSON_AddNumberToObject(response, "last_refresh", lastRefreshEpoch);
    cJSON_AddStringToObject(response, "state", uiMessage(uiState));
    cJSON_AddStringToObject(response, "error_detail", lastErrorDetail.c_str());
    // 屏幕上此刻实际显示的视图号：没有异常行时固定是视图 1，也不轮换。
    const bool rotating = anyRowAbnormal();
    cJSON_AddNumberToObject(response, "view_phase",
                            rotating ? quota::viewPhase(millis()) : 1);
    cJSON_AddBoolToObject(response, "rows_rotating", rotating);
    // 顶层字段保留兼容：当前显示的行里最早的一次自动抓取。
    uint32_t nextDue = 0;
    const uint32_t nowMs = millis();
    for (size_t i = 0; i < rowCount; ++i) {
      const SourceStatus* row = statusFor(rowConfigs[i].source);
      if (row == nullptr || row->dueAtMs == 0) continue;
      if (nextDue == 0 || static_cast<int32_t>(row->dueAtMs - nextDue) < 0) {
        nextDue = row->dueAtMs;
      }
    }
    const int32_t nextMs = static_cast<int32_t>(nextDue - nowMs);
    cJSON_AddNumberToObject(response, "next_refresh_seconds",
                            nextDue != 0 && nextMs > 0 ? nextMs / 1000 : 0);
    cJSON_AddNumberToObject(response, "wifi_status", static_cast<int>(WiFi.status()));
    cJSON_AddNumberToObject(response, "wifi_rssi",
                            WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
    cJSON_AddStringToObject(response, "wifi_ip", WiFi.localIP().toString().c_str());
    cJSON_AddBoolToObject(response, "deepseek_key_set", settings.deepseekConfigured());
    cJSON_AddBoolToObject(response, "openrouter_key_set", settings.openrouterConfigured());
    cJSON_AddNumberToObject(response, "deepseek_key_count",
                            static_cast<double>(deepseekBalances.count));
    cJSON_AddNumberToObject(response, "openrouter_key_count",
                            static_cast<double>(openrouterBalances.count));
    cJSON_AddNumberToObject(response, "i2c_address", kDisplayI2cAddress);
    cJSON_AddNumberToObject(response, "scl_gpio9", digitalRead(pins::kI2cClock));
    cJSON_AddNumberToObject(response, "sda_gpio8", digitalRead(pins::kI2cData));
  } else if (command == "refresh") {
    addResponseBase(response, requestId, true);
    refreshRequested = true;
  } else if (command == "erase") {
    clearSettings();
    drawScreen();
    addResponseBase(response, requestId, true);
    cJSON_AddStringToObject(response, "state", "未配置");
  } else {
    addResponseBase(response, requestId, false);
    cJSON_AddStringToObject(response, "error", "未知命令");
  }
  if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(response, "ok")) &&
      (command == "configure" || command == "login" || command == "claude_login" ||
       command == "antigravity_login" || command == "erase")) {
    char* reply = cJSON_PrintUnformatted(response);
    if (reply) {
      lastMutationId = requestId;
      lastMutationCommand = command;
      lastMutationReply = reply;
      cJSON_free(reply);
    }
  }
  sendResponse(response);
  cJSON_Delete(response);
  cJSON_Delete(root);
}

void processSerial() {
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\n') {
      if (!serialLine.isEmpty()) handleSerialCommand(serialLine);
      serialLine = "";
    } else if (c != '\r') {
      if (serialLine.length() < kMaximumSerialCommandBytes) serialLine += c;
      else serialLine = "";
    }
  }
}

void updateKeys() {
  const uint32_t now = millis();
  for (auto& key : keys) {
    const bool pressed = digitalRead(key.pin) == LOW;
    if (pressed != key.rawPressed) {
      key.rawPressed = pressed;
      key.changedAtMs = now;
    }
    if (key.stablePressed != key.rawPressed && now - key.changedAtMs >= kDebounceMs) {
      key.stablePressed = key.rawPressed;
      Serial.printf("KEY %s %s\n", key.name, pressed ? "DOWN" : "UP");
      if (key.pin == pins::kKeyHash && key.stablePressed && settings.configured()) {
        refreshRequested = true;
      }
    }
  }
}
}  // namespace

void setup() {
  Serial.begin(kSerialBaud);
  serialLine.reserve(12000);
  delay(500);
  for (auto& key : keys) pinMode(key.pin, INPUT_PULLUP);
  displayMutex = xSemaphoreCreateMutex();
  display.setI2CAddress(kDisplayI2cAddress << 1);
  display.setBusClock(kI2cFrequency);
  display.begin();
  loadSettings();
  // 动画和互斥锁都是可选增强：任何一个建不出来就退化成「没有 1px 动画、照常绘图」，
  // 不要因为可选功能把整机拖进 panic 或静默半失效。
  if (displayMutex == nullptr) {
    Serial.println("ERROR display mutex");
  } else if (xTaskCreatePinnedToCore(loadingAnimationTask, "loading-bar", 4096, nullptr, 1,
                                     nullptr, 0) != pdPASS) {
    Serial.println("ERROR loading task");
    vSemaphoreDelete(displayMutex);
    displayMutex = nullptr;
  }
  uiState = settings.configured() ? UiState::kConnecting : UiState::kUnconfigured;
  drawScreen();
  Serial.printf("READY OLED=0x%02X SCL=%u SDA=%u\n",
                kDisplayI2cAddress, pins::kI2cClock, pins::kI2cData);
  if (settings.configured()) refreshRequested = true;
}

void loop() {
  processSerial();
  updateKeys();
  const uint32_t now = millis();
  if (settings.configured()) {
    // 三个来源各自到期；手动刷新（按键 / fetchNow）让它们同时尝试。
    const DueSources due = dueSources(refreshRequested, now);
    if (anyDue(due)) {
      refreshRequested = false;
      // 刷新进行中不算异常，屏幕上不出现「正在刷新」，也不做多余的全屏重画。
      refreshSources(due);
      syncOverallState();
      drawScreen();
    }
  }
  // 有异常行时视图按相位轮换；没有异常行时两个视图相同，不必重画。
  if (anyRowAbnormal() && quota::viewPhase(millis()) != drawnPhase) {
    drawScreen();
  }
  if (static_cast<int32_t>(now - nextRedrawAtMs) >= 0) {
    drawScreen();
    // Schedule from the end of the transfer. If the OLED does not ACK, each
    // I2C chunk can consume the Wire timeout; scheduling from stale `now`
    // would otherwise turn this into a tight blocking loop and starve Serial.
    nextRedrawAtMs = millis() + kIdleRedrawMs;
  }
  delay(5);
}
