#pragma once

#include <cstddef>
#include <cstdio>

#include "balance_protocol.h"

// 128×64 屏幕上同一平台的多个余额用 + 连接。单个 key 沿用两位小数；
// 两个及以上时每个金额只保留一位小数，例如 12.0+24.0。
namespace balance_format {
inline bool amount(double value, bool oneDecimal, char* out, size_t size) {
  const int written = std::snprintf(out, size, oneDecimal ? "%.1f" : "%.2f", value);
  return written > 0 && static_cast<size_t>(written) < size;
}

inline bool joined(const balance_protocol::Balance* balances, size_t count,
                   char* out, size_t size) {
  if (out == nullptr || size == 0) return false;
  if (count == 0) {
    const int written = std::snprintf(out, size, "--");
    return written > 0 && static_cast<size_t>(written) < size;
  }
  const bool oneDecimal = count >= 2;
  size_t used = 0;
  for (size_t i = 0; i < count; ++i) {
    char part[32];
    if (balances[i].present) {
      if (!amount(balances[i].remaining, oneDecimal, part, sizeof(part))) return false;
    } else {
      std::snprintf(part, sizeof(part), "--");
    }
    const int written = std::snprintf(out + used, size - used,
                                      i == 0 ? "%s" : "+%s", part);
    if (written < 0 || static_cast<size_t>(written) >= size - used) return false;
    used += static_cast<size_t>(written);
  }
  return true;
}
}  // namespace balance_format
