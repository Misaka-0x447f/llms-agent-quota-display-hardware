#pragma once

#include <cJSON.h>
#include <cmath>
#include <cstdint>

#include "claude_protocol.h"

namespace codex {
using claude::RateWindow;

constexpr uint32_t kFiveHourSeconds = 5 * 3600;
constexpr uint32_t kSevenDaySeconds = 7 * 86400;

// 单个窗口：used_percent 必填（≥0，超过 100 按 100 截断），limit_window_seconds 必填且为正整数，
// reset_at 可省略/为 null（未知）；出现时必须是合理的 Unix 秒。
inline bool parseRateWindow(const cJSON* value, RateWindow& out) {
  if (!value || cJSON_IsNull(value)) {
    out = RateWindow{};
    return true;
  }
  if (!cJSON_IsObject(value)) return false;
  const cJSON* used = cJSON_GetObjectItemCaseSensitive(value, "used_percent");
  const cJSON* reset = cJSON_GetObjectItemCaseSensitive(value, "reset_at");
  const cJSON* duration = cJSON_GetObjectItemCaseSensitive(value, "limit_window_seconds");
  if (!cJSON_IsNumber(used) || !std::isfinite(used->valuedouble) ||
      used->valuedouble < 0) {
    return false;
  }
  if (!claude::integer(duration, 1, static_cast<double>(UINT32_MAX))) return false;
  RateWindow candidate;
  candidate.present = true;
  // 超额使用可能让上游返回 >100；剩余量按 0 处理，不能因此丢掉整份响应。
  candidate.usedPercent =
      used->valuedouble > 100 ? 100.0f : static_cast<float>(used->valuedouble);
  candidate.durationSeconds = static_cast<uint32_t>(duration->valuedouble);
  if (reset && !cJSON_IsNull(reset)) {
    if (!cJSON_IsNumber(reset) || !std::isfinite(reset->valuedouble) ||
        reset->valuedouble < 0 || reset->valuedouble > UINT32_MAX) {
      return false;
    }
    candidate.resetAt = static_cast<uint32_t>(reset->valuedouble);
  }
  out = candidate;
  return true;
}

// 用量响应：rate_limit.primary_window / secondary_window，按窗口长度归到 5 小时或 7 天。
// 只认这两种长度；其他长度忽略（接口以后可能新增窗口，不该让整次解析失败）。
// 两个窗口都没有时返回 false，调用方保留上一次成功的数据。
inline bool parseUsage(const char* json, RateWindow& five, RateWindow& seven) {
  cJSON* root = cJSON_ParseWithOpts(json, nullptr, true);
  if (!root) return false;
  const cJSON* rateLimit = cJSON_GetObjectItemCaseSensitive(root, "rate_limit");
  RateWindow newFive, newSeven;
  bool sawFive = false;
  bool sawSeven = false;
  bool valid = cJSON_IsObject(rateLimit);
  if (valid) {
    const char* names[] = {"primary_window", "secondary_window"};
    for (const char* name : names) {
      const cJSON* value = cJSON_GetObjectItemCaseSensitive(rateLimit, name);
      if (value == nullptr) continue;
      RateWindow candidate;
      if (!parseRateWindow(value, candidate)) {
        valid = false;
        break;
      }
      if (!candidate.present) continue;
      if (candidate.durationSeconds == kFiveHourSeconds) {
        newFive = candidate;
        sawFive = true;
      } else if (candidate.durationSeconds == kSevenDaySeconds) {
        newSeven = candidate;
        sawSeven = true;
      }
    }
  }
  cJSON_Delete(root);
  if (!valid || (!sawFive && !sawSeven)) return false;
  five = newFive;
  seven = newSeven;
  return true;
}
}  // namespace codex
