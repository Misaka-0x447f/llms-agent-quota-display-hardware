#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "refresh_plan.h"

namespace quota {
// 屏幕最多 3 行；每行由一个「来源 + 窗口」描述，顺序即行序。
enum class RowSource : uint8_t {
  kChatgpt,
  kClaude,
  kDeepseek,
  kOpenrouter,
};

enum class WindowKind : uint8_t {
  kSevenDay,
  kFiveHour,
};

constexpr size_t kMaxRows = 3;
constexpr char kDefaultRows[] = "chatgpt:7d,deepseek,openrouter";

// 每行可以单独用 `@5m` / `@30s` 覆盖默认节奏（窗口 10 分钟、余额 50 秒，见 refresh_plan.h）。
constexpr uint32_t kMinRefreshSeconds = 10;
constexpr uint32_t kMaxRefreshSeconds = 24 * 3600;

struct RowConfig {
  RowSource source;
  WindowKind window;  // 只对 chatgpt/claude 有意义，余额行固定填 kSevenDay。
  uint32_t intervalSeconds;  // 已解析好的刷新间隔（默认值也会填进来）。
};

inline bool isWindowSource(RowSource source) {
  return source == RowSource::kChatgpt || source == RowSource::kClaude;
}

inline const char* rowSourceName(RowSource source) {
  switch (source) {
    case RowSource::kChatgpt: return "chatgpt";
    case RowSource::kClaude: return "claude";
    case RowSource::kDeepseek: return "deepseek";
    case RowSource::kOpenrouter: return "openrouter";
  }
  return "";
}

inline const char* windowSuffix(WindowKind window) {
  return window == WindowKind::kFiveHour ? "5h" : "7d";
}

inline uint32_t defaultIntervalSeconds(RowSource source) {
  return isWindowSource(source) ? kWindowRefreshSeconds : kBalanceRefreshSeconds;
}

// 间隔写法：整数 + 单位（s/m/h），例如 30s、10m、1h；范围 10 秒～24 小时。
inline bool parseDuration(const char* text, size_t length, uint32_t& seconds) {
  if (text == nullptr || length < 2) return false;
  const char unit = text[length - 1];
  const uint32_t scale = unit == 's' ? 1 : unit == 'm' ? 60 : unit == 'h' ? 3600 : 0;
  if (scale == 0) return false;
  uint64_t value = 0;
  for (size_t i = 0; i + 1 < length; ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
    value = value * 10 + static_cast<uint64_t>(text[i] - '0');
    if (value > kMaxRefreshSeconds) return false;
  }
  const uint64_t total = value * scale;
  if (total < kMinRefreshSeconds || total > kMaxRefreshSeconds) return false;
  seconds = static_cast<uint32_t>(total);
  return true;
}

inline bool formatDurationSeconds(uint32_t seconds, char* out, size_t size) {
  if (out == nullptr || size == 0) return false;
  int written = 0;
  if (seconds % 3600 == 0) {
    written = snprintf(out, size, "%uh", seconds / 3600);
  } else if (seconds % 60 == 0) {
    written = snprintf(out, size, "%um", seconds / 60);
  } else {
    written = snprintf(out, size, "%us", seconds);
  }
  return written > 0 && static_cast<size_t>(written) < size;
}

inline bool parseSource(const char* name, size_t length, RowSource& source) {
  struct Entry {
    const char* name;
    RowSource source;
  };
  static const Entry entries[] = {
      {"chatgpt", RowSource::kChatgpt},
      {"claude", RowSource::kClaude},
      {"deepseek", RowSource::kDeepseek},
      {"openrouter", RowSource::kOpenrouter},
  };
  for (const Entry& entry : entries) {
    if (strlen(entry.name) == length && strncmp(entry.name, name, length) == 0) {
      source = entry.source;
      return true;
    }
  }
  return false;
}

// 语法：`source[:window][@interval]`，逗号分隔，最多 3 项，忽略各项两侧的空格。
// 窗口只接受 7d / 5h 且只能跟在 chatgpt / claude 后面；余额行写窗口视为错误。
// 间隔形如 `@5m` / `@30s`，省略时按来源取默认值（窗口 10 分钟、余额 50 秒）。
// 解析是全有或全无的：任一项非法就返回 false，不留下半份配置。
inline bool parseRows(const char* text, size_t length, RowConfig* out, size_t& count) {
  if (text == nullptr || out == nullptr) return false;
  count = 0;
  RowConfig parsed[kMaxRows];
  size_t parsedCount = 0;
  size_t index = 0;
  while (index <= length) {
    size_t end = index;
    while (end < length && text[end] != ',') ++end;
    size_t begin = index;
    size_t tail = end;
    while (begin < tail && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
    while (tail > begin && (text[tail - 1] == ' ' || text[tail - 1] == '\t')) --tail;
    if (tail == begin) return false;
    const char* token = text + begin;
    size_t tokenLength = tail - begin;
    uint32_t interval = 0;
    bool hasInterval = false;
    const char* at = static_cast<const char*>(memchr(token, '@', tokenLength));
    if (at != nullptr) {
      const size_t atIndex = static_cast<size_t>(at - token);
      const size_t intervalLength = tokenLength - atIndex - 1;
      if (!parseDuration(at + 1, intervalLength, interval)) return false;
      hasInterval = true;
      tokenLength = atIndex;
    }
    const char* colon = static_cast<const char*>(memchr(token, ':', tokenLength));
    const size_t nameLength = colon == nullptr ? tokenLength
                                               : static_cast<size_t>(colon - token);
    RowSource source = RowSource::kChatgpt;
    if (!parseSource(token, nameLength, source)) return false;
    WindowKind window = WindowKind::kSevenDay;
    if (colon != nullptr) {
      if (!isWindowSource(source)) return false;
      const char* suffix = colon + 1;
      const size_t suffixLength = tokenLength - nameLength - 1;
      if (suffixLength == 2 && strncmp(suffix, "5h", 2) == 0) {
        window = WindowKind::kFiveHour;
      } else if (!(suffixLength == 2 && strncmp(suffix, "7d", 2) == 0)) {
        return false;
      }
    }
    if (parsedCount >= kMaxRows) return false;
    parsed[parsedCount].source = source;
    parsed[parsedCount].window = window;
    parsed[parsedCount].intervalSeconds =
        hasInterval ? interval : defaultIntervalSeconds(source);
    ++parsedCount;
    if (end == length) break;
    index = end + 1;
    if (index == length) return false;  // 末尾多一个逗号
  }
  if (parsedCount == 0) return false;
  for (size_t i = 0; i < parsedCount; ++i) out[i] = parsed[i];
  count = parsedCount;
  return true;
}

// 同一个来源可能占多行（例如 chatgpt:5h 和 chatgpt:7d），抓取节奏取其中最短的，
// 这样两行都不会比用户设定的更旧。
inline uint32_t sourceIntervalSeconds(const RowConfig* rows, size_t count,
                                      RowSource source) {
  uint32_t interval = 0;
  for (size_t i = 0; i < count; ++i) {
    if (rows[i].source != source) continue;
    if (interval == 0 || rows[i].intervalSeconds < interval) {
      interval = rows[i].intervalSeconds;
    }
  }
  return interval == 0 ? defaultIntervalSeconds(source) : interval;
}

// 回显给 `status` 与 CLI 的规范写法，例如
// `chatgpt:7d@10m,deepseek@50s,openrouter@50s`：每行都带上该行配置的间隔，
// 读的人不用再记默认值（同一来源占多行时抓取取最短，回显仍保留各行配置值）。
inline bool formatRows(const RowConfig* rows, size_t count, char* out, size_t size) {
  if (rows == nullptr || out == nullptr || size == 0 || count == 0 || count > kMaxRows) {
    return false;
  }
  out[0] = '\0';
  size_t used = 0;
  for (size_t i = 0; i < count; ++i) {
    char interval[8] = {};
    if (!formatDurationSeconds(rows[i].intervalSeconds, interval, sizeof(interval))) {
      out[0] = '\0';
      return false;
    }
    const int written = snprintf(out + used, size - used, "%s%s%s@%s",
                                 i == 0 ? "" : ",", rowSourceName(rows[i].source),
                                 isWindowSource(rows[i].source)
                                     ? (rows[i].window == WindowKind::kFiveHour ? ":5h" : ":7d")
                                     : "",
                                 interval);
    if (written < 0 || static_cast<size_t>(written) >= size - used) {
      // 失败时不留半截配置，调用方看到的是空串。
      out[0] = '\0';
      return false;
    }
    used += static_cast<size_t>(written);
  }
  return true;
}

// 行内文字的实际墨迹范围：7×14 的 `[` 顶到基线 −11，6×12 的 p 伸到基线 +1，
// 视图 2 的中文 12×12 也顶到 −11；loading 线放在基线 +2，正好压在本行墨迹之下。
constexpr int16_t kTextTopRows = 11;
constexpr int16_t kTextBottomRows = 1;
constexpr int16_t kLoadingLineOffset = 2;

struct RowSlotLayout {
  int16_t textY;  // 基线
  int16_t lineY;  // loading 线
};

// 行数少于 3 时整体居中：3 行 20/36/52，2 行 28/44，1 行 36；行距保持 16px。
// 每条线落在独立的 8px 页里，动画刷新一次只发一页。
inline bool rowLayout(size_t count, RowSlotLayout* out) {
  if (out == nullptr || count == 0 || count > kMaxRows) return false;
  static const int16_t kThree[] = {20, 36, 52};
  static const int16_t kTwo[] = {28, 44};
  static const int16_t kOne[] = {36};
  const int16_t* baselines = count == 3 ? kThree : count == 2 ? kTwo : kOne;
  for (size_t i = 0; i < count; ++i) {
    out[i].textY = baselines[i];
    out[i].lineY = static_cast<int16_t>(baselines[i] + kLoadingLineOffset);
  }
  return true;
}

// 行布局必须同时满足：线压在本行墨迹之下、不越过下一行墨迹、不出屏、三线不同页。
inline bool rowLayoutFits(size_t count, const RowSlotLayout* layout) {
  if (layout == nullptr || count == 0 || count > kMaxRows) return false;
  for (size_t i = 0; i < count; ++i) {
    if (layout[i].lineY <= layout[i].textY + kTextBottomRows) return false;
    if (layout[i].lineY + 1 > 64) return false;
    if (i + 1 < count && layout[i].lineY + 1 > layout[i + 1].textY - kTextTopRows) {
      return false;
    }
    for (size_t j = 0; j < i; ++j) {
      if (layout[i].lineY / 8 == layout[j].lineY / 8) return false;
    }
  }
  return true;
}
}  // namespace quota
