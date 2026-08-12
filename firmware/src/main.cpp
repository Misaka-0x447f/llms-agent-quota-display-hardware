#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <cJSON.h>
#include <time.h>

#include "ui_glyphs.h"

namespace pins {
constexpr uint8_t kI2cClock = 9;
constexpr uint8_t kI2cData = 8;
constexpr uint8_t kKeyStar = 3;
constexpr uint8_t kKeyHash = 2;
constexpr uint8_t kKeyDown = 1;
constexpr uint8_t kKeyUp = 0;
}  // namespace pins

namespace {
constexpr uint32_t kSerialBaud = 115200;
constexpr uint32_t kI2cFrequency = 100000;
constexpr uint32_t kDebounceMs = 25;
constexpr uint32_t kIdleRedrawMs = 60000;
constexpr uint32_t kFiveHourSeconds = 5UL * 60 * 60;
constexpr uint32_t kSevenDaySeconds = 7UL * 24 * 60 * 60;
constexpr uint32_t kMinimumValidEpoch = 1700000000UL;
constexpr uint16_t kMinimumRefreshMinutes = 1;
constexpr uint16_t kMaximumRefreshMinutes = 1440;
constexpr uint32_t kInitialRetryDelaySeconds = 4;
constexpr uint32_t kMaximumRetryDelaySeconds = 60;
constexpr size_t kMaximumSerialCommandBytes = 64 * 1024;
constexpr char kClientId[] = "app_EMoamEEZ73f0CkXaXp7hrann";
constexpr char kTokenUrl[] = "https://auth.openai.com/oauth/token";
constexpr char kUsageUrl[] = "https://chatgpt.com/backend-api/wham/usage";

extern const uint8_t rootca_crt_bundle_start[]
    asm("_binary_data_cert_x509_crt_bundle_bin_start");

U8G2_SSD1315_128X64_NONAME_F_SW_I2C display(
    U8G2_R0, pins::kI2cClock, pins::kI2cData, U8X8_PIN_NONE);
Preferences preferences;

enum class UiState : uint8_t {
  kUnconfigured,
  kConfigError,
  kConnecting,
  kLoading,
  kReady,
  kWifiNoNetwork,
  kWifiConnectionFailed,
  kWifiTimeout,
  kWifiDisconnected,
  kTimeSyncError,
  kDnsError,
  kTlsError,
  kTcpRefused,
  kRequestTimeout,
  kConnectionLost,
  kTransportError,
  kAuthError,
  kRateLimitError,
  kServerError,
  kHttpError,
  kDataError,
};

struct Settings {
  String wifiSsid;
  String wifiPassword;
  String accessToken;
  String refreshToken;
  String accountId;
  uint64_t expiresAt = 0;
  uint16_t refreshMinutes = 5;
  int16_t utcOffsetMinutes = 0;

  bool credentialsPresent() const {
    return !wifiSsid.isEmpty() && !accessToken.isEmpty() &&
           !refreshToken.isEmpty() && !accountId.isEmpty();
  }

  bool refreshIntervalValid() const {
    return refreshMinutes >= kMinimumRefreshMinutes &&
           refreshMinutes <= kMaximumRefreshMinutes;
  }

  bool configured() const {
    return credentialsPresent() && refreshIntervalValid();
  }

  bool configurationInvalid() const {
    return credentialsPresent() && !refreshIntervalValid();
  }
};

struct RateWindow {
  bool present = false;
  float usedPercent = 0;
  uint32_t resetAt = 0;
  uint32_t durationSeconds = 0;
};

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
UiState uiState = UiState::kUnconfigured;
String lastErrorDetail;
Key keys[] = {
    {"K4(*)", pins::kKeyStar, false, false, 0},
    {"K3(#)", pins::kKeyHash, false, false, 0},
    {"K2(DOWN)", pins::kKeyDown, false, false, 0},
    {"K1(UP)", pins::kKeyUp, false, false, 0},
};
String serialLine;
uint32_t lastRefreshEpoch = 0;
uint32_t nextRefreshAtMs = 0;
uint32_t nextRedrawAtMs = 0;
uint32_t retryDelaySeconds = kInitialRetryDelaySeconds;
bool refreshRequested = false;
constexpr uint8_t kDisplayI2cAddress = 0x3c;

uint32_t epochNow() {
  const time_t value = time(nullptr);
  return value >= static_cast<time_t>(kMinimumValidEpoch)
             ? static_cast<uint32_t>(value)
             : 0;
}

const char* uiMessage(UiState state) {
  switch (state) {
    case UiState::kUnconfigured: return "未配置";
    case UiState::kConfigError: return "配置无效";
    case UiState::kConnecting: return "正在联网";
    case UiState::kLoading: return "正在刷新";
    case UiState::kWifiNoNetwork: return "WiFi无此网络";
    case UiState::kWifiConnectionFailed: return "WiFi连接失败";
    case UiState::kWifiTimeout: return "WiFi连接超时";
    case UiState::kWifiDisconnected: return "WiFi已断开";
    case UiState::kTimeSyncError: return "时间同步失败";
    case UiState::kDnsError: return "DNS失败";
    case UiState::kTlsError: return "TLS失败";
    case UiState::kTcpRefused: return "TCP被拒绝";
    case UiState::kRequestTimeout: return "请求超时";
    case UiState::kConnectionLost: return "连接已断开";
    case UiState::kTransportError: return "HTTP传输失败";
    case UiState::kAuthError: return "登录失效";
    case UiState::kRateLimitError: return "请求过频";
    case UiState::kServerError: return "服务器错误";
    case UiState::kHttpError: return "HTTP错误";
    case UiState::kDataError: return "数据异常";
    case UiState::kReady: return "";
  }
  return "数据异常";
}

void setError(UiState state, const String& detail) {
  uiState = state;
  lastErrorDetail = detail;
}

void scheduleRetry() {
  nextRefreshAtMs = millis() + retryDelaySeconds * 1000UL;
  retryDelaySeconds = min(kMaximumRetryDelaySeconds, retryDelaySeconds * 2);
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

void loadSettings() {
  preferences.begin("codexquota", false);
  settings.wifiSsid = preferences.getString("wifi_ssid");
  settings.wifiPassword = preferences.getString("wifi_pass");
  settings.accessToken = preferences.getString("access");
  settings.refreshToken = preferences.getString("refresh");
  settings.accountId = preferences.getString("account");
  settings.expiresAt = preferences.getULong64("expires", 0);
  settings.refreshMinutes = preferences.getUShort("period", 5);
  settings.utcOffsetMinutes = preferences.getShort("utc_off", 0);
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
  ok &= preferences.putString("access", settings.accessToken) > 0;
  ok &= preferences.putString("refresh", settings.refreshToken) > 0;
  ok &= preferences.putString("account", settings.accountId) > 0;
  ok &= preferences.putULong64("expires", settings.expiresAt) > 0;
  ok &= preferences.putUShort("period", settings.refreshMinutes) > 0;
  ok &= preferences.putShort("utc_off", settings.utcOffsetMinutes) > 0;
  return ok;
}

void clearSettings() {
  preferences.clear();
  settings = Settings{};
  fiveHour = RateWindow{};
  sevenDay = RateWindow{};
  lastRefreshEpoch = 0;
  nextRefreshAtMs = 0;
  retryDelaySeconds = kInitialRetryDelaySeconds;
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

bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
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
  configTime(0, 0, "pool.ntp.org", "time.cloudflare.com", "time.google.com");
  const uint32_t timeDeadline = millis() + 10000;
  while (epochNow() == 0 && static_cast<int32_t>(millis() - timeDeadline) < 0) {
    delay(100);
  }
  if (epochNow() == 0) {
    setError(UiState::kTimeSyncError, "NTP 时间同步失败（未取得有效时间）");
    return false;
  }
  return true;
}

bool jsonString(cJSON* object, const char* key, String& destination) {
  cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
  if (!cJSON_IsString(value) || value->valuestring == nullptr) return false;
  destination = value->valuestring;
  return true;
}

bool refreshAccessToken() {
  if (!connectWiFi()) return false;
  if (!resolveHost("auth.openai.com")) return false;
  WiFiClientSecure client;
  client.setCACertBundle(rootca_crt_bundle_start);
  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(15000);
  if (!http.begin(client, kTokenUrl)) {
    setError(UiState::kTransportError, "OAuth 请求初始化失败");
    return false;
  }
  http.addHeader("Accept", "application/json");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body = "client_id=" + formEncode(kClientId) +
                "&grant_type=refresh_token&refresh_token=" +
                formEncode(settings.refreshToken) +
                "&scope=openid%20profile%20email";
  const int status = http.POST(body);
  const String response = http.getString();
  http.end();
  if (status <= 0) {
    setError(transportState(status, client), transportDetail("OAuth", status, client));
    return false;
  }
  if (status != 200) {
    setError(status == 400 ? UiState::kAuthError : classifyHttpStatus(status),
             "OAuth 服务返回 HTTP " + String(status));
    return false;
  }

  cJSON* root = cJSON_ParseWithLength(response.c_str(), response.length());
  if (root == nullptr) {
    setError(UiState::kDataError, "OAuth 响应不是有效 JSON");
    return false;
  }
  String accessToken;
  String rotatedRefreshToken;
  cJSON* expiresIn = cJSON_GetObjectItemCaseSensitive(root, "expires_in");
  bool valid = jsonString(root, "access_token", accessToken) &&
               cJSON_IsNumber(expiresIn) && expiresIn->valuedouble > 0;
  jsonString(root, "refresh_token", rotatedRefreshToken);
  if (valid) {
    settings.accessToken = accessToken;
    if (!rotatedRefreshToken.isEmpty()) settings.refreshToken = rotatedRefreshToken;
    settings.expiresAt = static_cast<uint64_t>(epochNow()) +
                         static_cast<uint64_t>(expiresIn->valuedouble);
    if (!saveSettings()) valid = false;
  }
  cJSON_Delete(root);
  if (!valid) setError(UiState::kDataError, "OAuth 响应缺少 access_token 或 expires_in");
  return valid;
}

bool parseRateWindow(cJSON* value, RateWindow& output) {
  if (!cJSON_IsObject(value)) return false;
  cJSON* used = cJSON_GetObjectItemCaseSensitive(value, "used_percent");
  cJSON* resetAt = cJSON_GetObjectItemCaseSensitive(value, "reset_at");
  cJSON* duration = cJSON_GetObjectItemCaseSensitive(value, "limit_window_seconds");
  if (!cJSON_IsNumber(used) || !cJSON_IsNumber(duration)) return false;
  output.present = true;
  output.usedPercent = constrain(static_cast<float>(used->valuedouble), 0.0f, 100.0f);
  output.durationSeconds = static_cast<uint32_t>(duration->valuedouble);
  output.resetAt = cJSON_IsNumber(resetAt)
                       ? static_cast<uint32_t>(resetAt->valuedouble)
                       : 0;
  return true;
}

bool parseUsage(const String& response) {
  cJSON* root = cJSON_ParseWithLength(response.c_str(), response.length());
  if (root == nullptr) return false;
  cJSON* rateLimit = cJSON_GetObjectItemCaseSensitive(root, "rate_limit");
  RateWindow parsedFive;
  RateWindow parsedSeven;
  if (cJSON_IsObject(rateLimit)) {
    const char* names[] = {"primary_window", "secondary_window"};
    for (const char* name : names) {
      RateWindow candidate;
      if (!parseRateWindow(cJSON_GetObjectItemCaseSensitive(rateLimit, name), candidate)) {
        continue;
      }
      if (candidate.durationSeconds == kFiveHourSeconds) parsedFive = candidate;
      if (candidate.durationSeconds == kSevenDaySeconds) parsedSeven = candidate;
    }
  }
  cJSON_Delete(root);
  if (!parsedFive.present && !parsedSeven.present) return false;
  fiveHour = parsedFive;
  sevenDay = parsedSeven;
  return true;
}

int getUsage(String& response) {
  if (!resolveHost("chatgpt.com")) return -1;
  WiFiClientSecure client;
  client.setCACertBundle(rootca_crt_bundle_start);
  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(15000);
  if (!http.begin(client, kUsageUrl)) {
    setError(UiState::kTransportError, "额度请求初始化失败");
    return -1;
  }
  http.addHeader("Accept", "application/json");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + settings.accessToken);
  http.addHeader("Chatgpt-Account-Id", settings.accountId);
  http.addHeader("User-Agent", "codex_cli_rs/0.76.0 (ESP32-C3; quota-display)");
  const int status = http.GET();
  response = http.getString();
  http.end();
  if (status <= 0) {
    setError(transportState(status, client), transportDetail("额度请求", status, client));
  }
  return status;
}

bool refreshQuota() {
  uiState = UiState::kLoading;
  lastErrorDetail = "";
  if (!connectWiFi()) return false;
  const uint32_t now = epochNow();
  if (settings.expiresAt == 0 || settings.expiresAt <= static_cast<uint64_t>(now) + 120) {
    if (!refreshAccessToken()) return false;
  }

  String response;
  int status = getUsage(response);
  if (status == 401) {
    if (!refreshAccessToken()) return false;
    status = getUsage(response);
  }
  if (status <= 0) {
    if (lastErrorDetail.isEmpty()) {
      setError(UiState::kTransportError, "额度请求未取得 HTTP 响应");
    }
    return false;
  }
  if (status != 200) {
    setError(classifyHttpStatus(status), "额度服务返回 HTTP " + String(status));
    return false;
  }
  if (!parseUsage(response)) {
    setError(UiState::kDataError, "额度响应 JSON 缺少可识别的 5h/7d 窗口");
    return false;
  }

  lastRefreshEpoch = epochNow();
  nextRefreshAtMs = millis() + static_cast<uint32_t>(settings.refreshMinutes) * 60000UL;
  retryDelaySeconds = kInitialRetryDelaySeconds;
  uiState = UiState::kReady;
  lastErrorDetail = "";
  return true;
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

void drawWindowLine(int16_t y, const RateWindow& window) {
  if (!window.present) return;
  const uint32_t now = epochNow();
  const uint32_t remainSeconds = window.resetAt > now ? window.resetAt - now : 0;
  const float remaining = 100.0f - window.usedPercent;
  const String label = window.resetAt ? formatDuration(remainSeconds) : "7d";
  String prefix = "[" + label + "]  " +
                  String(static_cast<int>(roundf(remaining))) + "%";
  display.setFont(u8g2_font_7x14_tf);
  display.drawStr(0, y, prefix.c_str());

  const int16_t x = display.getStrWidth(prefix.c_str()) + 4;
  drawDeltaGlyph(x, y);
  if (window.resetAt == 0 || window.durationSeconds == 0) {
    display.drawStr(x + 9, y, "--");
    return;
  }
  const float expectedRemaining = remaining *
                                  static_cast<float>(window.durationSeconds) / 100.0f;
  const float rawDelta = static_cast<float>(remainSeconds) - expectedRemaining;
  const char sign = rawDelta >= 0 ? '-' : '+';
  const uint32_t magnitude = static_cast<uint32_t>(fabsf(rawDelta));
  const String delta = String(sign) + formatDuration(magnitude, magnitude >= 86400);
  display.drawStr(x + 9, y, delta.c_str());
}

String formatLastRefresh() {
  const uint32_t now = epochNow();
  if (lastRefreshEpoch == 0 || now == 0 || now < lastRefreshEpoch) return "--";
  const uint32_t elapsedSeconds = now - lastRefreshEpoch;
  if (elapsedSeconds < 60) return "<1m 前";
  return String(elapsedSeconds / 60) + "m 前";
}

void drawScreen() {
  display.clearBuffer();
  drawWindowLine(20, sevenDay);

  display.setFont(u8g2_font_6x12_tf);
  const String lastRefresh = formatLastRefresh();
  drawUiText(0, 41, lastRefresh.c_str());

  const char* status = uiMessage(uiState);
  if (status[0] != '\0') {
    const int16_t width = uiTextWidth(status);
    drawUiText(max<int16_t>(0, 128 - width), 41, status);
  }
  display.sendBuffer();
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
  cJSON* response = cJSON_CreateObject();

  if (requestId.isEmpty() || command.isEmpty()) {
    addResponseBase(response, requestId, false);
    cJSON_AddStringToObject(response, "error", "命令格式错误");
  } else if (command == "configure") {
    Settings candidate = settings;
    cJSON* refreshMinutes = cJSON_GetObjectItemCaseSensitive(root, "refresh_minutes");
    cJSON* utcOffset = cJSON_GetObjectItemCaseSensitive(root, "utc_offset_minutes");
    cJSON* expiresAt = cJSON_GetObjectItemCaseSensitive(root, "expires_at");
    const bool authFieldsProvided =
        cJSON_GetObjectItemCaseSensitive(root, "access_token") != nullptr ||
        cJSON_GetObjectItemCaseSensitive(root, "refresh_token") != nullptr ||
        cJSON_GetObjectItemCaseSensitive(root, "account_id") != nullptr ||
        expiresAt != nullptr;
    const bool valid =
        jsonString(root, "wifi_ssid", candidate.wifiSsid) &&
        jsonString(root, "wifi_password", candidate.wifiPassword) &&
        cJSON_IsNumber(refreshMinutes) &&
        refreshMinutes->valuedouble >= kMinimumRefreshMinutes &&
        refreshMinutes->valuedouble <= kMaximumRefreshMinutes &&
        refreshMinutes->valuedouble ==
            static_cast<uint16_t>(refreshMinutes->valuedouble) &&
        cJSON_IsNumber(utcOffset) &&
        utcOffset->valuedouble >= -840 && utcOffset->valuedouble <= 840 &&
        !candidate.wifiSsid.isEmpty() &&
        (!authFieldsProvided ||
         (jsonString(root, "access_token", candidate.accessToken) &&
          jsonString(root, "refresh_token", candidate.refreshToken) &&
          jsonString(root, "account_id", candidate.accountId) &&
          cJSON_IsNumber(expiresAt) && expiresAt->valuedouble > 0 &&
          !candidate.accessToken.isEmpty() && !candidate.refreshToken.isEmpty() &&
          !candidate.accountId.isEmpty()));
    if (valid) {
      candidate.refreshMinutes = static_cast<uint16_t>(refreshMinutes->valuedouble);
      candidate.utcOffsetMinutes = static_cast<int16_t>(utcOffset->valuedouble);
      if (authFieldsProvided) {
        candidate.expiresAt = static_cast<uint64_t>(expiresAt->valuedouble);
      }
      settings = candidate;
      if (saveSettings()) {
        addResponseBase(response, requestId, true);
        cJSON_AddNumberToObject(response, "refresh_minutes", settings.refreshMinutes);
        WiFi.disconnect(true, false);
        refreshRequested = true;
        retryDelaySeconds = kInitialRetryDelaySeconds;
        lastErrorDetail = "";
      } else {
        addResponseBase(response, requestId, false);
        cJSON_AddStringToObject(response, "error", "配置写入失败");
      }
    } else {
      addResponseBase(response, requestId, false);
      cJSON_AddStringToObject(response, "error", "配置字段无效");
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
        retryDelaySeconds = kInitialRetryDelaySeconds;
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
    cJSON_AddBoolToObject(response, "configured", settings.configured());
    cJSON_AddBoolToObject(response, "wifi_connected", WiFi.status() == WL_CONNECTED);
    cJSON_AddNumberToObject(response, "refresh_minutes", settings.refreshMinutes);
    cJSON_AddNumberToObject(response, "last_refresh", lastRefreshEpoch);
    const int32_t nextMs = static_cast<int32_t>(nextRefreshAtMs - millis());
    cJSON_AddNumberToObject(response, "next_refresh_seconds",
                            nextRefreshAtMs != 0 && nextMs > 0 ? nextMs / 1000 : 0);
    cJSON_AddStringToObject(response, "state", uiMessage(uiState));
    cJSON_AddStringToObject(response, "error_detail", lastErrorDetail.c_str());
    cJSON_AddNumberToObject(response, "wifi_status", static_cast<int>(WiFi.status()));
    cJSON_AddNumberToObject(response, "wifi_rssi",
                            WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
    cJSON_AddStringToObject(response, "wifi_ip", WiFi.localIP().toString().c_str());
    cJSON_AddNumberToObject(response, "i2c_address", kDisplayI2cAddress);
    cJSON_AddNumberToObject(response, "scl_gpio9", digitalRead(pins::kI2cClock));
    cJSON_AddNumberToObject(response, "sda_gpio8", digitalRead(pins::kI2cData));
  } else if (command == "refresh") {
    addResponseBase(response, requestId, true);
    refreshRequested = true;
  } else if (command == "erase") {
    clearSettings();
    addResponseBase(response, requestId, true);
    cJSON_AddStringToObject(response, "state", "未配置");
  } else {
    addResponseBase(response, requestId, false);
    cJSON_AddStringToObject(response, "error", "未知命令");
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
  uiState = settings.configurationInvalid()
      ? UiState::kConfigError
      : (settings.configured() ? UiState::kConnecting : UiState::kUnconfigured);
  drawScreen();
  Serial.printf("READY OLED=0x%02X SCL=%u SDA=%u\n",
                kDisplayI2cAddress, pins::kI2cClock, pins::kI2cData);
  if (settings.configured()) refreshRequested = true;
}

void loop() {
  processSerial();
  updateKeys();
  const uint32_t now = millis();
  if (settings.configured() && nextRefreshAtMs != 0 &&
      static_cast<int32_t>(now - nextRefreshAtMs) >= 0) {
    refreshRequested = true;
  }
  if (refreshRequested && settings.configured()) {
    refreshRequested = false;
    uiState = UiState::kLoading;
    drawScreen();
    refreshQuota();
    if (uiState != UiState::kReady) {
      scheduleRetry();
    }
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
