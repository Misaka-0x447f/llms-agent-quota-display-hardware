#pragma once

#include <cJSON.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "claude_protocol.h"

// Antigravity（Google Cloud Code）每周额度与充值余额（AI Credits）的解析。
//
// 只读接口（与 agy 自己的调用一致，均为 POST + Bearer）：
//   /v1internal:retrieveUserQuotaSummary  配额分组（bucket = gemini-weekly / 3p-weekly …）
//   /v1internal:loadCodeAssist            订阅档位 + paidTier.availableCredits（AI Credits 余额）
// 请求体是 `{}`，不需要 project：个人账号的 Gemini 组不依赖项目。
namespace antigravity {
using claude::RateWindow;

constexpr uint32_t kSevenDaySeconds = 7 * 86400;
// 用户 2026-11-25 确认：100 credits = 1 美元，显示时直接 ÷100。
constexpr double kCreditsPerUsd = 100.0;

struct Credits {
  bool present = false;
  double amount = 0;   // 已换算成美元
  double minimum = 0;  // 低于它就不能再用于计费
};

// 响应可能被包一层 {"response": {...}}（不同通道的行为不同）。
inline const cJSON* unwrap(const cJSON* root) {
  if (!cJSON_IsObject(root)) return nullptr;
  const cJSON* wrapped = cJSON_GetObjectItemCaseSensitive(root, "response");
  return cJSON_IsObject(wrapped) ? wrapped : root;
}

// protobuf JSON 用小驼峰，部分通道回 snake_case；两种都接受。
inline const cJSON* field(const cJSON* object, const char* camel, const char* snake) {
  if (!cJSON_IsObject(object)) return nullptr;
  const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, camel);
  if (value == nullptr && snake != nullptr) {
    value = cJSON_GetObjectItemCaseSensitive(object, snake);
  }
  return value;
}

inline bool number(const cJSON* value, double& out) {
  // int64 在 proto3 JSON 里是字符串（"25"），数字也接受。
  if (cJSON_IsNumber(value) && std::isfinite(value->valuedouble)) {
    out = value->valuedouble;
    return true;
  }
  if (!cJSON_IsString(value) || value->valuestring == nullptr) return false;
  char* end = nullptr;
  const double parsed = std::strtod(value->valuestring, &end);
  if (end == value->valuestring || (end != nullptr && *end != '\0') ||
      !std::isfinite(parsed)) {
    return false;
  }
  out = parsed;
  return true;
}

inline bool text(const cJSON* value, char* out, size_t size, const char* fallback = "") {
  const char* source = cJSON_IsString(value) && value->valuestring != nullptr
                           ? value->valuestring
                           : fallback;
  const size_t length = std::strlen(source);
  if (length + 1 > size) return false;
  std::memcpy(out, source, length + 1);
  return true;
}

// 单个配额桶 → 窗口。remainingFraction 是**剩余**比例；只有 resetTime 没有
// fraction 的桶表示「已耗尽」，按 100% 已用处理，而不是当成未知丢弃。
inline bool bucketWindow(const cJSON* bucket, RateWindow& out) {
  if (!cJSON_IsObject(bucket)) return false;
  const cJSON* reset = field(bucket, "resetTime", "reset_time");
  const cJSON* fraction = field(bucket, "remainingFraction", "remaining_fraction");
  RateWindow candidate;
  candidate.present = true;
  candidate.durationSeconds = kSevenDaySeconds;
  bool sawFraction = false;
  if (fraction != nullptr && !cJSON_IsNull(fraction)) {
    double value = 0;
    if (!number(fraction, value) || value < 0 || value > 1) return false;
    candidate.usedPercent = static_cast<float>((1.0 - value) * 100.0);
    sawFraction = true;
  }
  if (reset != nullptr && !cJSON_IsNull(reset)) {
    if (!cJSON_IsString(reset) || !claude::parseTimestamp(reset->valuestring, candidate.resetAt)) {
      return false;
    }
  }
  if (!sawFraction) {
    // 没有剩余比例时，只有带重置时间的桶才有意义（= 已耗尽）。
    if (!candidate.resetAt) return false;
    candidate.usedPercent = 100.0f;
  } else if (candidate.usedPercent < 0) {
    candidate.usedPercent = 0;
  } else if (candidate.usedPercent > 100) {
    candidate.usedPercent = 100;
  }
  out = candidate;
  return true;
}

// 取 Gemini 模型组的每周桶。
//
// 只认精确的 `gemini-weekly`：上游还有分模型/分档位的桶（例如 `gemini-4-argon-weekly`），
// 没有任何证据表明它们等于「Gemini 模型组」的共享每周额度，拿它们顶替会让屏幕显示
// 一份用户并没有在看的额度。找不到就返回 false，这一行显示占位符。
inline bool parseQuotaSummary(const char* json, RateWindow& seven) {
  cJSON* root = cJSON_ParseWithOpts(json, nullptr, true);
  if (root == nullptr) return false;
  const cJSON* body = unwrap(root);
  const cJSON* groups = field(body, "groups", nullptr);
  bool found = false;
  RateWindow window;
  if (cJSON_IsArray(groups)) {
    const cJSON* group = nullptr;
    cJSON_ArrayForEach(group, groups) {
      const cJSON* buckets = field(group, "buckets", nullptr);
      if (!cJSON_IsArray(buckets)) continue;
      const cJSON* bucket = nullptr;
      cJSON_ArrayForEach(bucket, buckets) {
        const cJSON* id = field(bucket, "bucketId", "bucket_id");
        if (!cJSON_IsString(id) || std::strcmp(id->valuestring, "gemini-weekly") != 0) continue;
        if (bucketWindow(bucket, window)) {
          found = true;
          break;
        }
      }
      if (found) break;
    }
  }
  cJSON_Delete(root);
  if (found) seven = window;
  return found;
}

// 把响应里实际出现的 bucketId 摘出来（最多 4 个），用于排查「响应里没有
// gemini-weekly」这种失败：错误详情里带上它，比只说「缺少」可操作。
inline bool describeBuckets(const char* json, char* out, size_t size) {
  if (out == nullptr || size == 0) return false;
  out[0] = '\0';
  cJSON* root = cJSON_ParseWithOpts(json, nullptr, true);
  if (root == nullptr) {
    std::snprintf(out, size, "响应不是 JSON");
    return true;
  }
  const cJSON* body = unwrap(root);
  const cJSON* groups = field(body, "groups", nullptr);
  if (!cJSON_IsArray(groups)) {
    std::snprintf(out, size, "没有 groups");
    cJSON_Delete(root);
    return true;
  }
  size_t used = 0;
  int count = 0;
  const cJSON* group = nullptr;
  cJSON_ArrayForEach(group, groups) {
    const cJSON* buckets = field(group, "buckets", nullptr);
    if (!cJSON_IsArray(buckets)) continue;
    const cJSON* bucket = nullptr;
    cJSON_ArrayForEach(bucket, buckets) {
      const cJSON* id = field(bucket, "bucketId", "bucket_id");
      if (!cJSON_IsString(id) || id->valuestring == nullptr) continue;
      if (count > 0 && used < size) {
        out[used++] = ',';
      }
      if (used < size) {
        const int written = std::snprintf(out + used, size - used, "%s", id->valuestring);
        if (written > 0) used += static_cast<size_t>(written);
      }
      if (++count >= 4) break;
    }
    if (count >= 4) break;
  }
  if (count == 0) std::snprintf(out, size, "groups 里没有 bucketId");
  if (used >= size) out[size - 1] = '\0';
  cJSON_Delete(root);
  return true;
}

// 订阅档位：paidTier 优先（免费账号也会报 currentTier = free-tier）。
inline bool parseProject(const char* json, char* out, size_t size) {
  cJSON* root = cJSON_ParseWithOpts(json, nullptr, true);
  if (root == nullptr) return false;
  const cJSON* body = unwrap(root);
  const bool ok = text(field(body, "cloudaicompanionProject", "cloudaicompanion_project"), out,
                       size) && out[0] != '\0';
  cJSON_Delete(root);
  return ok;
}

inline bool parsePlan(const char* json, char* out, size_t size) {
  if (out == nullptr || size == 0) return false;
  cJSON* root = cJSON_ParseWithOpts(json, nullptr, true);
  if (root == nullptr) return false;
  const cJSON* body = unwrap(root);
  bool ok = false;
  const char* keys[] = {"paidTier", "currentTier"};
  const char* snakeKeys[] = {"paid_tier", "current_tier"};
  for (size_t i = 0; i < 2 && !ok; ++i) {
    const cJSON* tier = field(body, keys[i], snakeKeys[i]);
    if (cJSON_IsString(tier)) {
      ok = text(tier, out, size) && out[0] != '\0';
    } else if (cJSON_IsObject(tier)) {
      ok = text(field(tier, "name", nullptr), out, size) && out[0] != '\0';
    }
  }
  cJSON_Delete(root);
  return ok;
}

// 充值余额：paidTier.availableCredits[] 里 creditType == GOOGLE_ONE_AI 的那一项。
//
// 三态是必须的：调用方要区分「上游明确说没有余额」（清空旧值、屏幕显示 `--`）和
// 「这次响应无效」（保留旧值），否则一次截断的响应就会把真实余额抹掉。
enum class CreditsStatus : uint8_t {
  kAbsent,   // 合法响应，明确没有 GOOGLE_ONE_AI 项
  kFound,    // 拿到了可用的余额
  kInvalid,  // 响应本身无效（空/截断/不是对象/金额坏掉）
};

inline CreditsStatus parseCredits(const char* json, Credits& out) {
  cJSON* root = cJSON_ParseWithOpts(json, nullptr, true);
  if (root == nullptr) return CreditsStatus::kInvalid;
  if (!cJSON_IsObject(root)) {
    cJSON_Delete(root);
    return CreditsStatus::kInvalid;
  }
  const cJSON* wrapped = cJSON_GetObjectItemCaseSensitive(root, "response");
  if (wrapped != nullptr && !cJSON_IsObject(wrapped)) {
    // `{"response": []}` / `{"response": null}`：包装坏了，不能当成「明确没有余额」。
    cJSON_Delete(root);
    return CreditsStatus::kInvalid;
  }
  const cJSON* body = unwrap(root);
  if (!cJSON_IsObject(body)) {
    cJSON_Delete(root);
    return CreditsStatus::kInvalid;
  }
  const cJSON* tier = field(body, "paidTier", "paid_tier");
  if (tier != nullptr && !cJSON_IsNull(tier) && !cJSON_IsObject(tier)) {
    // `"paidTier": []` / `"paidTier": "x"`：字段在但类型错，属于坏响应，
    // 不能当成「明确没有余额」而清掉上一次的真实金额。
    cJSON_Delete(root);
    return CreditsStatus::kInvalid;
  }
  const cJSON* list = field(tier, "availableCredits", "available_credits");
  if (list != nullptr && !cJSON_IsNull(list) && !cJSON_IsArray(list)) {
    cJSON_Delete(root);
    return CreditsStatus::kInvalid;
  }
  CreditsStatus status = CreditsStatus::kAbsent;
  if (cJSON_IsArray(list)) {
    const cJSON* entry = nullptr;
    cJSON_ArrayForEach(entry, list) {
      if (!cJSON_IsObject(entry)) {
        // 列表里混进 null/字符串：坏响应，不当成「没有这一项」。
        status = CreditsStatus::kInvalid;
        break;
      }
      const cJSON* type = field(entry, "creditType", "credit_type");
      const char* creditType = cJSON_IsString(type) ? type->valuestring : "";
      if (std::strcmp(creditType, "GOOGLE_ONE_AI") != 0) continue;
      double amount = 0;
      double minimum = 0;
      bool hadAmount = false;
      const cJSON* amountValue = field(entry, "creditAmount", "credit_amount");
      if (amountValue != nullptr && !cJSON_IsNull(amountValue)) {
        if (!number(amountValue, amount) || amount < 0) {
          status = CreditsStatus::kInvalid;
          continue;
        }
        hadAmount = true;
      }
      // 真实响应里「余额 0」就是只给 minimum 不给 creditAmount。只有这种
      // （amount 缺失 + minimum 有效）才按 0 处理；两个字段都缺、或只有
      // creditType 的条目仍算坏数据，避免不认识的形状把已知余额清掉。
      bool hadMinimum = false;
      const cJSON* minimumValue =
          field(entry, "minimumCreditAmountForUsage", "minimum_credit_amount_for_usage");
      if (minimumValue != nullptr && !cJSON_IsNull(minimumValue)) {
        if (!number(minimumValue, minimum) || minimum < 0) {
          status = CreditsStatus::kInvalid;
          continue;
        }
        hadMinimum = true;
      }
      if (!hadAmount && !hadMinimum) {
        status = CreditsStatus::kInvalid;
        continue;
      }
      Credits candidate;
      candidate.present = true;
      candidate.amount = amount / kCreditsPerUsd;
      candidate.minimum = minimum / kCreditsPerUsd;
      out = candidate;
      status = CreditsStatus::kFound;
      break;
    }
  }
  cJSON_Delete(root);
  return status;
}

// 这一行是否该从「剩余百分比」切到「充值余额」：**只在周额度用尽时**（用户
// 2026-11-25 撤回「余额低于 minimum 也切换」的决定——那个规则会让没买 credits 的
// 账号永远看不到配速）。
// `credits` 不参与判断，只用于决定切过去之后显示什么；保留参数是为了让调用点
// 的语义清楚：这一行是因为额度耗尽才进入充值形态。
inline bool overageMode(const RateWindow& seven, const Credits& credits) {
  (void)credits;
  if (!seven.present) return false;
  // 与屏幕上的取整保持一致：剩余比例四舍五入到 0% 才切换。用 > 99.5 而不是
  // >= 99.5，避免「显示 1% 却已经显示余额」这种自相矛盾的帧。
  return seven.usedPercent > 99.5f;
}
}  // namespace antigravity
