#pragma once

#include <stdint.h>
#include <cJSON.h>

namespace quota {
constexpr uint32_t kDefaultRefreshSeconds = 50;
constexpr uint32_t kMinimumRefreshSeconds = 50;
constexpr uint32_t kMaximumRefreshSeconds = 86400;

inline bool validRefreshSeconds(uint32_t seconds) {
  return seconds >= kMinimumRefreshSeconds && seconds <= kMaximumRefreshSeconds;
}

// Accept the legacy integer-minute command, but never ambiguous dual units.
// Missing fields preserve the current interval; invalid fields never modify it.
inline bool readRefreshSeconds(const cJSON* root, uint32_t& seconds) {
  const cJSON* sec = cJSON_GetObjectItemCaseSensitive(root, "refresh_seconds");
  const cJSON* min = cJSON_GetObjectItemCaseSensitive(root, "refresh_minutes");
  if (sec && min) return false;
  if (!sec && !min) return true;
  const cJSON* value = sec ? sec : min;
  const double lower = sec ? kMinimumRefreshSeconds : 1;
  const double upper = sec ? kMaximumRefreshSeconds : 1440;
  if (!cJSON_IsNumber(value) || !(value->valuedouble >= lower && value->valuedouble <= upper)) {
    return false;
  }
  const uint32_t integer = static_cast<uint32_t>(value->valuedouble);
  if (value->valuedouble != integer) return false;
  seconds = sec ? integer : integer * 60;
  return true;
}
}  // namespace quota
