#pragma once

#include <cJSON.h>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace balance_protocol {
struct Balance {
  double remaining = 0.0;
  const char* currency = "USD";
  bool present = false;
};

// Accept decimal strings, without strtod's permissive whitespace, hex or NaN.
inline bool decimal(const cJSON* value, bool allowString, double& out) {
  double parsed;
  if (cJSON_IsNumber(value)) {
    parsed = value->valuedouble;
  } else if (allowString && cJSON_IsString(value) && value->valuestring) {
    const char* text = value->valuestring;
    const char* p = text;
    if (*p == '-') ++p;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') ++p;
    if (*p == '.') {
      ++p;
      if (*p < '0' || *p > '9') return false;
      while (*p >= '0' && *p <= '9') ++p;
    }
    if (*p == 'e' || *p == 'E') {
      ++p;
      if (*p == '+' || *p == '-') ++p;
      if (*p < '0' || *p > '9') return false;
      while (*p >= '0' && *p <= '9') ++p;
    }
    if (*p) return false;
    errno = 0;
    char* end = nullptr;
    parsed = std::strtod(text, &end);
    if (errno == ERANGE || !end || *end) return false;
  } else {
    return false;
  }
  if (!std::isfinite(parsed)) return false;
  out = parsed;
  return true;
}

inline bool parseDeepSeek(const char* json, Balance& out) {
  if (!json) return false;
  cJSON* root = cJSON_ParseWithOpts(json, nullptr, true);
  if (!root) return false;
  const cJSON* infos = cJSON_GetObjectItemCaseSensitive(root, "balance_infos");
  const cJSON* cny = nullptr;
  if (cJSON_IsObject(root) && cJSON_IsArray(infos)) {
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, infos) {
      const cJSON* currency = cJSON_GetObjectItemCaseSensitive(item, "currency");
      if (!cJSON_IsObject(item) || !cJSON_IsString(currency)) continue;
      if (std::strcmp(currency->valuestring, "CNY") == 0) cny = item;
    }
  }
  Balance candidate;
  candidate.currency = "CNY";
  const bool valid = cny && decimal(
      cJSON_GetObjectItemCaseSensitive(cny, "total_balance"), true,
      candidate.remaining);
  cJSON_Delete(root);
  if (!valid) return false;
  candidate.present = true;
  out = candidate;  // Only replace a last-good value after complete validation.
  return true;
}

inline bool parseOpenRouter(const char* json, Balance& out) {
  if (!json) return false;
  cJSON* root = cJSON_ParseWithOpts(json, nullptr, true);
  if (!root) return false;
  const cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
  double credits = 0.0, usage = 0.0;
  const bool valid = cJSON_IsObject(root) && cJSON_IsObject(data) &&
      decimal(cJSON_GetObjectItemCaseSensitive(data, "total_credits"), false, credits) &&
      decimal(cJSON_GetObjectItemCaseSensitive(data, "total_usage"), false, usage);
  cJSON_Delete(root);
  if (!valid || !std::isfinite(credits - usage)) return false;
  Balance candidate;
  candidate.remaining = credits - usage;
  candidate.currency = "USD";
  candidate.present = true;
  out = candidate;
  return true;
}
}  // namespace balance_protocol
