#pragma once

#include <stdint.h>

namespace quota {
// 每行有两个视图：视图 1 是该行的数据，视图 2 是该行的过期时间与状态词。
// 只有异常行才有视图 2，且所有行共用同一个相位，保证同一时刻看的是同一个视图号。
// 空闲、刷新中、未配置都不是异常，这些状态下每行只有视图 1（等价于两个视图相同）。
constexpr uint32_t kViewHoldMs = 4000;

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

// 只有异常行在相位 2 切到视图 2；其余情况一律显示视图 1。
inline bool showStatusView(bool hasError, uint8_t phase) {
  return hasError && phase == 2;
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
