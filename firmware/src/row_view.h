#pragma once

#include <stdint.h>

namespace quota {
// 每行有两个视图：视图 1 是该行的数据，视图 2 是该行的过期时间与状态词。
// 只有异常行才有视图 2，且所有行共用同一个相位，保证同一时刻看的是同一个视图号。
// 空闲、刷新中、未配置都不是异常，这些状态下每行只有视图 1（等价于两个视图相同）。
// 每个视图停留 2 秒（用户 2026-11-13 确认的节奏；实现曾用 4 秒）。
constexpr uint32_t kViewHoldMs = 2000;

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

inline const char* uiMessage(UiState state) {
  switch (state) {
    case UiState::kUnconfigured: return "未配置";
    case UiState::kConfigError: return "配置无效";
    case UiState::kConnecting: return "正在联网";
    case UiState::kLoading: return "正在刷新";
    case UiState::kWifiNoNetwork: return "WiFi无此网络";
    case UiState::kWifiConnectionFailed: return "WiFi连接失败";
    case UiState::kWifiTimeout: return "WiFi连接超时";
    case UiState::kWifiDisconnected: return "WiFi已断开";
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
  return "";
}

// 异常指这一行有需要告知用户的失败；进行态和未配置都没有可告知的内容。
inline bool abnormal(UiState state) {
  switch (state) {
    case UiState::kReady:
    case UiState::kLoading:
    case UiState::kConnecting:
    case UiState::kUnconfigured:
      return false;
    case UiState::kConfigError:
    case UiState::kWifiNoNetwork:
    case UiState::kWifiConnectionFailed:
    case UiState::kWifiTimeout:
    case UiState::kWifiDisconnected:
    case UiState::kDnsError:
    case UiState::kTlsError:
    case UiState::kTcpRefused:
    case UiState::kRequestTimeout:
    case UiState::kConnectionLost:
    case UiState::kTransportError:
    case UiState::kAuthError:
    case UiState::kRateLimitError:
    case UiState::kServerError:
    case UiState::kHttpError:
    case UiState::kDataError:
      return true;
  }
  return false;
}

inline uint8_t viewPhase(uint32_t nowMs) {
  return static_cast<uint8_t>(((nowMs / kViewHoldMs) & 1U) + 1U);
}

// 只有异常行才可能切到视图 2；其余情况一律显示视图 1。
// 这一行还没有任何数据时（hasData 为 false）不存在可以轮换的视图 1：这时常驻
// 视图 2，否则屏幕会在「空白」和「状态词」之间来回闪，看起来像故障。
inline bool showStatusView(bool hasError, bool hasData, uint8_t phase) {
  if (!hasError) return false;
  return !hasData || phase == 2;
}

// 视图 2 的横向排版：状态词贴右，其余元素贴左，彼此不重叠也不越过整行宽度。
// 宽度一律以像素传入（ASCII 6px、中文 12px）。放不下时按
// 平台名 → * → 过期时间 的顺序依次让位，状态词永远完整。
constexpr int16_t kStatusStarWidth = 6;
constexpr int16_t kStatusLabelGap = 4;
constexpr int16_t kStatusStateGap = 2;

struct StatusLineLayout {
  bool showLabel;
  bool showStar;
  bool showAge;
  int16_t labelX;
  int16_t starX;
  int16_t ageX;
  int16_t stateX;
};

// 正在刷新时在该行下面画 1px 的 indeterminate 进度条（Material 风格）：
// 一段固定宽度的高光从左侧外面扫到右侧外面，循环往复。只占 1px 高度。
constexpr uint32_t kLoadingSweepMs = 2400;
constexpr int16_t kLoadingBarWidth = 32;

struct LoadingBarSpan {
  int16_t x;
  int16_t width;
};

inline LoadingBarSpan loadingBarSpan(uint32_t nowMs, int16_t screenWidth) {
  const int16_t travel = static_cast<int16_t>(screenWidth + kLoadingBarWidth);
  const int32_t offset =
      static_cast<int32_t>((nowMs % kLoadingSweepMs) * travel / kLoadingSweepMs);
  LoadingBarSpan span{};
  span.x = static_cast<int16_t>(offset - kLoadingBarWidth);
  span.width = kLoadingBarWidth;
  return span;
}

// 一页 128 字节在软件 I²C 上要 ~110 ms，所以只重发高光扫过的那几列 tile。
// 新旧位置不相交时分成两段，避免扫到最右回卷那一帧退化成整页。
constexpr uint8_t kLoadingMaxFlushes = 2;

struct LoadingBarTiles {
  uint8_t tx;
  uint8_t tw;
};

struct LoadingBarFlush {
  uint8_t count;
  LoadingBarTiles spans[kLoadingMaxFlushes];
};

// 把像素区间 [left, right) 换算成 tile 区间；完全在屏幕外时返回 false。
inline bool loadingBarTilesForRange(int16_t left, int16_t right, int16_t screenWidth,
                                    LoadingBarTiles& tiles) {
  if (left < 0) left = 0;
  if (right > screenWidth) right = screenWidth;
  if (right <= left) return false;
  tiles.tx = static_cast<uint8_t>(left / 8);
  tiles.tw = static_cast<uint8_t>((right - 1) / 8 - tiles.tx + 1);
  return true;
}

inline bool loadingBarSpanToTiles(int16_t from, int16_t barWidth, int16_t screenWidth,
                                  LoadingBarTiles& tiles) {
  return loadingBarTilesForRange(from, static_cast<int16_t>(from + barWidth), screenWidth,
                                 tiles);
}

// 整段重画：覆盖新旧高光的并集。起步、状态切换后用这一种。
inline LoadingBarFlush loadingBarFlush(int16_t previousX, int16_t currentX, int16_t barWidth,
                                       int16_t screenWidth) {
  LoadingBarFlush flush{};
  const int16_t left = previousX < currentX ? previousX : currentX;
  const int16_t right =
      static_cast<int16_t>((previousX > currentX ? previousX : currentX) + barWidth);
  LoadingBarTiles tiles{};
  if (loadingBarTilesForRange(left, right, screenWidth, tiles)) {
    flush.count = 1;
    flush.spans[0] = tiles;
  }
  return flush;
}

// 增量重画：只发两端变化的部分（高光是一段实心条，中间每帧都没变）。
// 两块相交时是「一端擦掉、一端补上」；完全不相交（回卷或跳变）时是
// 「旧位置擦掉、新位置画上」，各一两个 tile，不会退化成整页。
inline LoadingBarFlush loadingBarFlushDiff(int16_t previousX, int16_t currentX, int16_t barWidth,
                                           int16_t screenWidth) {
  LoadingBarFlush flush{};
  const int16_t previousRight = static_cast<int16_t>(previousX + barWidth);
  const int16_t currentRight = static_cast<int16_t>(currentX + barWidth);
  LoadingBarTiles tiles{};
  const auto addSpan = [&](int16_t left, int16_t right) {
    if (loadingBarTilesForRange(left, right, screenWidth, tiles)) {
      flush.spans[flush.count++] = tiles;
    }
  };
  if (currentX >= previousX && currentX < previousRight) {
    addSpan(previousX, currentX);   // 左端露出的旧像素
    addSpan(previousRight, currentRight);  // 右端新增的高光
  } else if (currentX < previousX && currentRight > previousX) {
    addSpan(currentX, previousX);   // 左端新增的高光
    addSpan(currentRight, previousRight);  // 右端露出的旧像素
  } else {
    addSpan(previousX, previousRight);  // 不相交：擦旧
    addSpan(currentX, currentRight);    // 画新
  }
  return flush;
}

inline StatusLineLayout statusLineLayout(int16_t width, int16_t stateWidth,
                                         int16_t labelWidth, int16_t ageWidth,
                                         bool stale) {
  StatusLineLayout layout{};
  layout.stateX = stateWidth < width ? static_cast<int16_t>(width - stateWidth) : 0;
  const int16_t limit = static_cast<int16_t>(layout.stateX - kStatusStateGap);
  if (limit <= 0) return layout;  // 状态词占满整行，其它元素都不画
  const int16_t star = stale ? kStatusStarWidth : 0;
  const int16_t named = labelWidth > 0 ? labelWidth + kStatusLabelGap : 0;
  if (named > 0 && named + star + ageWidth <= limit) {
    layout.showLabel = true;
    layout.showStar = stale;
    layout.showAge = true;
    layout.labelX = 0;
    layout.starX = static_cast<int16_t>(named);
    layout.ageX = static_cast<int16_t>(named + star);
    return layout;
  }
  if (star + ageWidth <= limit) {
    layout.showStar = stale;
    layout.showAge = true;
    layout.starX = 0;
    layout.ageX = star;
    return layout;
  }
  if (ageWidth <= limit) {
    layout.showAge = true;
    layout.ageX = 0;
  }
  return layout;
}
}  // namespace quota
