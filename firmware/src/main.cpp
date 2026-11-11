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
#include "claude_protocol.h"
#include "freshness_format.h"
#include "refresh_plan.h"
#include "row_view.h"
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

extern const uint8_t rootca_crt_bundle_start[]
    asm("_binary_data_cert_x509_crt_bundle_bin_start");

U8G2_SSD1315_128X64_NONAME_F_SW_I2C display(
    U8G2_R0, pins::kI2cClock, pins::kI2cData, U8X8_PIN_NONE);
Preferences preferences;

struct Settings {
  String wifiSsid;
  String wifiPassword;
  String accessToken;
  String refreshToken;
  String accountId;
  String deepseekKeys;
  String openrouterKeys;
  uint64_t expiresAt = 0;
  int16_t utcOffsetMinutes = 0;

  bool codexConfigured() const {
    return !accessToken.isEmpty() && !refreshToken.isEmpty() && !accountId.isEmpty();
  }

  bool deepseekConfigured() const { return !deepseekKeys.isEmpty(); }

  bool openrouterConfigured() const { return !openrouterKeys.isEmpty(); }

  // 本版本只采集和显示 Claude；保留旧平台凭据但不再轮询旧数据源。
  bool credentialsPresent() const {
    return !wifiSsid.isEmpty() && claudeAuth.configured();
  }

  bool configured() const { return credentialsPresent(); }
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
SourceStatus claudeSource;
BalanceGroup deepseekBalances;
BalanceGroup openrouterBalances;
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

// 成功：记下数据新鲜度，按该来源固定的间隔排下一次。
void sourceSucceeded(SourceStatus& source, bool claude) {
  source.state = UiState::kReady;
  source.detail = "";
  source.lastOkMs = millis();
  source.retryDelaySeconds = kInitialRetryDelaySeconds;
  source.dueAtMs = millis() + quota::refreshIntervalSeconds(claude) * 1000UL;
  lastRefreshEpoch = epochNow();
}

// 失败：按该来源自己的间隔做退避；Claude 还要尊重服务端的 Retry-After 冷却。
void scheduleSourceRetry(SourceStatus& source, bool claude) {
  if (claude && claudeAuth.pending) {
    // 凭据已失效：不再自动重试，避免连续触发 token 轮换。
    source.dueAtMs = 0;
    return;
  }
  uint32_t wait = quota::retryDelay(claude, source.retryDelaySeconds);
  if (claude) {
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
void sourceFailed(SourceStatus& source, bool claude, UiState state, const String& detail) {
  source.state = state;
  source.detail = detail;
  scheduleSourceRetry(source, claude);
}

// 数据确实更新了、但这一行仍要显示失败（例如多 key 里只坏了一个）。
// 调度完全复用成功路径，避免继承已经翻倍过的退避；展示状态再覆盖回异常。
void sourcePartiallySucceeded(SourceStatus& source, bool claude, UiState state,
                              const String& detail) {
  sourceSucceeded(source, claude);
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
    if (jsonString(root, "access_token", candidate.accessToken) &&
        jsonString(root, "refresh_token", candidate.refreshToken) &&
        claude::integer(expires, kMinimumValidEpoch, UINT32_MAX) && candidate.configured()) {
      candidate.expiresAt = static_cast<uint64_t>(expires->valuedouble);
      jsonString(root, "provision_id", candidate.provisionId);
      candidate.pending = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "pending"));
      claudeAuth = candidate;
    }
    cJSON_Delete(root);
  }
  claudeBlockedUntil = preferences.getUInt("claude_wait", 0);
}

void resetBalanceGroup(BalanceGroup& group, const String& rawKeys) {
  group = BalanceGroup{};
  key_list::List keys;
  if (key_list::parse(rawKeys.c_str(), rawKeys.length(), keys) && keys.count > 0) {
    group.count = keys.count;
    group.status.state = UiState::kLoading;
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
  settings.utcOffsetMinutes = preferences.getShort("utc_off", 0);
  resetBalanceGroup(deepseekBalances, settings.deepseekKeys);
  resetBalanceGroup(openrouterBalances, settings.openrouterKeys);
  if (claudeAuth.configured()) claudeSource.state = UiState::kLoading;
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
  // Claude-only devices legitimately have no legacy Codex credentials.
  if (!settings.accessToken.isEmpty()) ok &= preferences.putString("access", settings.accessToken) > 0;
  if (!settings.refreshToken.isEmpty()) ok &= preferences.putString("refresh", settings.refreshToken) > 0;
  if (!settings.accountId.isEmpty()) ok &= preferences.putString("account", settings.accountId) > 0;
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
  ok &= preferences.putULong64("expires", settings.expiresAt) > 0;
  ok &= preferences.putShort("utc_off", settings.utcOffsetMinutes) > 0;
  return ok;
}

void clearSettings() {
  preferences.clear();
  settings = Settings{};
  claudeAuth = ClaudeAuth{};
  claudeBlockedUntil = 0;
  claudeSource = SourceStatus{};
  deepseekBalances = BalanceGroup{};
  openrouterBalances = BalanceGroup{};
  fiveHour = RateWindow{};
  sevenDay = RateWindow{};
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
  if (valid) candidate.expiresAt = static_cast<uint64_t>(epochNow()) +
                                  static_cast<uint64_t>(expires->valuedouble);
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
  if (claudeAuth.expiresAt <= static_cast<uint64_t>(epochNow()) + 120) {
    if (!refreshClaudeToken()) return false;
    refreshed = true;
  }
  String response;
  int status = getClaudeUsage(response);
  if (status == 401 && !refreshed) {
    if (!refreshClaudeToken()) return false;
    status = getClaudeUsage(response);
  }
  if (status != 200) {
    if (status > 0) {
      setError(classifyHttpStatus(status), "Claude 用量接口返回 HTTP " + String(status));
      if (status == 401 || status == 403) {
        // New credentials rejected, or scope missing: stop repeated token rotation.
        claudeAuth.pending = true;
        saveClaudeAuth(claudeAuth);
      }
    }
    return false;
  }
  if (!claude::parseUsage(response.c_str(), fiveHour, sevenDay)) {
    setError(UiState::kDataError, "Claude 用量响应缺少有效的 5h/7d 数据");
    return false;
  }
  sourceSucceeded(claudeSource, true);
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
                         bool (*parse)(const char*, balance_protocol::Balance&),
                         BalanceGroup& group) {
  key_list::List keys;
  if (!key_list::parse(rawKeys.c_str(), rawKeys.length(), keys)) {
    setError(UiState::kDataError, String(host) + " 余额 key 配置无效");
    group.count = 0;
    group.stale = true;
    sourceFailed(group.status, false, uiState, lastErrorDetail);
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
    sourceSucceeded(group.status, false);
    return;
  }
  // 部分 key 失败时错的那个由视图 1 的 * 标记、视图 2 显示它的状态，
  // 但这一行仍按 50 秒节奏继续抓，健康 key 的数据不能被一个坏 key 拖旧。
  if (decision.keepsSchedule) {
    sourcePartiallySucceeded(group.status, false, firstError, firstDetail);
    return;
  }
  sourceFailed(group.status, false, firstError, firstDetail);
}

// 三个来源各自排队，只刷新到期的那些；手动刷新时全部一起尝试。
struct DueSources {
  bool claude;
  bool deepseek;
  bool openrouter;
};

DueSources dueSources(bool manual, uint32_t nowMs) {
  DueSources due{};
  due.claude = claudeAuth.configured() &&
               (manual || quota::refreshDue(nowMs, claudeSource.dueAtMs));
  due.deepseek = settings.deepseekConfigured() &&
                 (manual || quota::refreshDue(nowMs, deepseekBalances.status.dueAtMs));
  due.openrouter = settings.openrouterConfigured() &&
                   (manual || quota::refreshDue(nowMs, openrouterBalances.status.dueAtMs));
  return due;
}

bool anyDue(const DueSources& due) {
  return due.claude || due.deepseek || due.openrouter;
}

// 全局状态只服务于既有串口语义：取第一个异常行，没有异常就是 kReady。
void syncOverallState() {
  const SourceStatus* rows[] = {
      &claudeSource, &deepseekBalances.status, &openrouterBalances.status};
  for (const SourceStatus* row : rows) {
    if (quota::abnormal(row->state)) {
      uiState = row->state;
      lastErrorDetail = row->detail;
      return;
    }
  }
  uiState = UiState::kReady;
  lastErrorDetail = "";
}

void refreshSources(const DueSources& due) {
  if (!anyDue(due)) return;
  if (!connectWiFi()) {
    // 联网失败时，本轮到期的每一行都显示同一个网络错误。
    if (due.claude) sourceFailed(claudeSource, true, uiState, lastErrorDetail);
    if (due.deepseek) sourceFailed(deepseekBalances.status, false, uiState, lastErrorDetail);
    if (due.openrouter) sourceFailed(openrouterBalances.status, false, uiState, lastErrorDetail);
    return;
  }
  if (due.claude) {
    if (!refreshClaudeUsage()) {
      sourceFailed(claudeSource, true, uiState, lastErrorDetail);
    }
  }
  if (due.deepseek) {
    refreshBalanceGroup("api.deepseek.com", "https://api.deepseek.com/user/balance",
                        settings.deepseekKeys, balance_protocol::parseDeepSeek,
                        deepseekBalances);
  }
  if (due.openrouter) {
    refreshBalanceGroup("openrouter.ai", "https://openrouter.ai/api/v1/credits",
                        settings.openrouterKeys, balance_protocol::parseOpenRouter,
                        openrouterBalances);
  }
}

String formatDuration(uint32_t seconds, bool extended = false) {
  struct Unit { uint32_t seconds; char suffix; };
  static const Unit units[] = {{86400, 'd'}, {3600, 'h'}, {60, 'm'}};
  for (size_t i = 0; i < 3; ++i) {
    if (seconds >= units[i].seconds || i == 2) {
      const uint32_t major = seconds / units[i].seconds;
      String result = String(major) + units[i].suffix;
      if (extended && i + 1 < 3) {
        const uint32_t minor = (seconds % units[i].seconds) / units[i + 1].seconds;
        if (minor > 0) result += String(minor) + units[i + 1].suffix;
      }
      return result;
    }
  }
  return "0m";
}

void drawDeltaGlyph(int16_t x, int16_t baseline) {
  // 细线框 Δ：避免实心三角形带来的粗重观感。
  display.drawLine(x + 3, baseline - 7, x, baseline);
  display.drawLine(x, baseline, x + 6, baseline);
  display.drawLine(x + 6, baseline, x + 3, baseline - 7);
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

void drawWindowLine(int16_t y, const RateWindow& window, bool rightAlignPercent = false) {
  if (!window.present) return;
  const uint32_t now = epochNow();
  const uint32_t remainSeconds = window.resetAt > now ? window.resetAt - now : 0;
  const float remaining = 100.0f - window.usedPercent;
  // 时钟未同步时 resetAt 无法换算成剩余时间，显示 -- 而不是把时间戳当天数。
  const String label = window.resetAt == 0
      ? String(window.durationSeconds == claude::kFiveHourSeconds ? "5h" : "7d")
      : (now == 0 ? String("--") : formatDuration(remainSeconds));
  const String percent = String(static_cast<int>(roundf(remaining))) + "%";
  const String prefix = "[" + label + "]" + (rightAlignPercent ? "" : "  " + percent);
  display.setFont(u8g2_font_7x14_tf);
  display.drawStr(0, y, prefix.c_str());
  if (rightAlignPercent) {
    display.drawStr(128 - display.getStrWidth(percent.c_str()), y, percent.c_str());
  }

  const int16_t x = display.getStrWidth(prefix.c_str()) + 4;
  drawDeltaGlyph(x, y);
  // 配速同样由 resetAt 与当前时间算出；时钟未就绪时两者都不可用。
  if (window.resetAt == 0 || window.durationSeconds == 0 || now == 0) {
    display.drawStr(x + 9, y, "--");
    return;
  }
  const float expectedRemaining = remaining *
                                  static_cast<float>(window.durationSeconds) / 100.0f;
  const float rawDelta = static_cast<float>(remainSeconds) - expectedRemaining;
  char minuteDelta[8];
  if (quota::formatMinuteDelta(-rawDelta, minuteDelta, sizeof(minuteDelta))) {
    display.drawStr(x + 9, y, minuteDelta);
    return;
  }
  const char sign = rawDelta >= 0 ? '-' : '+';
  const uint32_t magnitude = static_cast<uint32_t>(fabsf(rawDelta));
  const String delta = String(sign) + formatDuration(magnitude, magnitude >= 86400);
  display.drawStr(x + 9, y, delta.c_str());
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

// 沿用原来的行距和观感：三行仍相隔 16px，整屏上下各留半行（8px）空闲，
// 只是把原来底部状态行占的位置换成了与上下对称的白边。
constexpr int16_t kRowClaudeY = 20;
constexpr int16_t kRowDeepseekY = 36;
constexpr int16_t kRowOpenrouterY = 52;

void drawClaudeRow(int16_t y, uint8_t phase) {
  if (quota::showStatusView(quota::abnormal(claudeSource.state), phase)) {
    drawStatusRow(y, "Claude", false, claudeSource.lastOkMs, quota::kClaudeRefreshSeconds,
                  claudeSource.state);
    return;
  }
  drawWindowLine(y, sevenDay, true);
}

void drawBalanceRow(int16_t y, const char* label, const BalanceGroup& group, uint8_t phase) {
  const bool stale = group.stale && group.anyPresent();
  if (quota::showStatusView(quota::abnormal(group.status.state), phase)) {
    drawStatusRow(y, label, stale, group.status.lastOkMs, quota::kBalanceRefreshSeconds,
                  group.status.state);
    return;
  }
  drawBalanceLine(y, label, group);
}

// 没有任何异常行时两个视图完全相同，不需要为了轮换重画。
bool anyRowAbnormal() {
  return quota::abnormal(claudeSource.state) ||
         quota::abnormal(deepseekBalances.status.state) ||
         quota::abnormal(openrouterBalances.status.state);
}

void drawScreen() {
  display.clearBuffer();
  const uint8_t phase = quota::viewPhase(millis());
  drawClaudeRow(kRowClaudeY, phase);
  drawBalanceRow(kRowDeepseekY, "deepseek", deepseekBalances, phase);
  drawBalanceRow(kRowOpenrouterY, "openrouter", openrouterBalances, phase);
  display.sendBuffer();
  drawnPhase = phase;
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
    const bool keyFieldsProvided = deepseekProvided || openrouterProvided;
    const bool ssidProvided =
        cJSON_GetObjectItemCaseSensitive(root, "wifi_ssid") != nullptr;
    const bool passwordProvided =
        cJSON_GetObjectItemCaseSensitive(root, "wifi_password") != nullptr;

    // 部分更新：只校验并套用请求里出现的字段，其余保留设备原值。
    bool valid = ssidProvided || passwordProvided || utcOffset != nullptr ||
                 authFieldsProvided || keyFieldsProvided;
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
    if (authFieldsProvided) {
      valid = valid &&
              jsonString(root, "access_token", candidate.accessToken) &&
              jsonString(root, "refresh_token", candidate.refreshToken) &&
              jsonString(root, "account_id", candidate.accountId) &&
              cJSON_IsNumber(expiresAt) && expiresAt->valuedouble > 0 &&
              !candidate.accessToken.isEmpty() && !candidate.refreshToken.isEmpty() &&
              !candidate.accountId.isEmpty();
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
      if (authFieldsProvided) {
        candidate.expiresAt = static_cast<uint64_t>(expiresAt->valuedouble);
      }
      settings = candidate;
      if (saveSettings()) {
        addResponseBase(response, requestId, true);
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
      } else {
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
      lastRefreshEpoch = 0;
      // A fresh authorization must trigger an immediate fetch.
      refreshRequested = true;
      claudeSource.retryDelaySeconds = kInitialRetryDelaySeconds;
      claudeSource.dueAtMs = 0;
      lastErrorDetail = "";
      uiState = settings.configured() ? UiState::kLoading : UiState::kUnconfigured;
      addResponseBase(response, requestId, true);
      cJSON_AddStringToObject(response, "provider", "claude");
    } else {
      addResponseBase(response, requestId, false);
      cJSON_AddStringToObject(response, "error", valid ? "Claude 凭据写入失败" : "Claude 登录字段无效");
    }
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
      settings = candidate;
      if (saveSettings()) {
        addResponseBase(response, requestId, true);
        cJSON_AddStringToObject(response, "account_id", settings.accountId.c_str());
        WiFi.disconnect(true, false);
        refreshRequested = true;
        uiState = settings.configured() ? UiState::kLoading : UiState::kUnconfigured;
        lastErrorDetail = "";
      } else {
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
    addSourceStatus("claude_status", claudeSource);
    addSourceStatus("deepseek_status", deepseekBalances.status);
    addSourceStatus("openrouter_status", openrouterBalances.status);
    cJSON_AddBoolToObject(response, "configured", settings.configured());
    cJSON_AddBoolToObject(response, "codex_configured", settings.codexConfigured());
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
    // 顶层字段保留兼容：三个来源里最早的一次自动抓取。
    uint32_t nextDue = 0;
    const uint32_t nowMs = millis();
    const SourceStatus* rows[] = {
        &claudeSource, &deepseekBalances.status, &openrouterBalances.status};
    for (const SourceStatus* row : rows) {
      if (row->dueAtMs == 0) continue;
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
      (command == "configure" || command == "login" || command == "claude_login" || command == "erase")) {
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
  display.setI2CAddress(kDisplayI2cAddress << 1);
  display.setBusClock(kI2cFrequency);
  display.begin();
  loadSettings();
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
