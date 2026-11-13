#pragma once

#include <stdint.h>

namespace quota {
// 抓取节奏按数据本身的更新速度定：窗口类不需要频繁抓，余额变化快。
// 具体间隔由行配置决定（默认窗口 10 分钟、余额 50 秒，可按行覆盖）。
// 默认节奏：窗口类（ChatGPT/Claude）10 分钟，余额类 50 秒；行配置可逐行覆盖。
constexpr uint32_t kWindowRefreshSeconds = 600;
constexpr uint32_t kBalanceRefreshSeconds = 50;

// dueAt == 0 表示该来源不在队列里；用无符号差值比较，可自然处理 millis 回绕。
inline bool refreshDue(uint32_t nowMs, uint32_t dueAtMs) {
  return dueAtMs != 0 && static_cast<int32_t>(nowMs - dueAtMs) >= 0;
}

// 失败退避不短于该来源的正常间隔，避免 10 分钟一抓的来源变成连续重试。
inline uint32_t retryDelay(uint32_t intervalSeconds, uint32_t delaySeconds) {
  return delaySeconds < intervalSeconds ? intervalSeconds : delaySeconds;
}

// 一组 key 抓完后的调度选择：有 key 失败时这一行要显示异常，但只要还有 key
// 正常更新，整组就必须按正常间隔继续抓——否则一个长期失败的 key 会通过退避
// 把健康 key 的节奏一路拖到 15 分钟。
struct GroupDecision {
  bool showError;      // 视图 2 显示失败状态
  bool keepsSchedule;  // 下一次仍按该来源的正常间隔
};

// total == 0 表示这一行没有 key：这里按「无事可做」处理（不显示异常），
// 前提是 `configure` 只接受 keys.count > 0 的配置，NVS 里不会有解析出 0 个 key 的值。
inline GroupDecision groupDecision(unsigned long total, unsigned long failed) {
  GroupDecision decision{};
  decision.showError = failed > 0 && total > 0;
  decision.keepsSchedule = failed < total;
  return decision;
}

// 服务端的冷却截止时间是 Unix 时间。设备时钟还没同步（epochNow() == 0）时既
// 判断不了、也算不出剩余量：这时按“没有冷却”处理，否则一个过去的截止时间会被
// 当成几亿秒之后，把重试排到 7 天后。只有时钟有效时才认这个截止时间。
inline bool cooldownActive(bool clockValid, uint32_t blockedUntilEpoch, uint32_t nowEpoch) {
  return clockValid && blockedUntilEpoch > nowEpoch;
}

inline uint32_t cooldownRemainingSeconds(bool clockValid, uint32_t blockedUntilEpoch,
                                        uint32_t nowEpoch) {
  return cooldownActive(clockValid, blockedUntilEpoch, nowEpoch)
             ? blockedUntilEpoch - nowEpoch
             : 0;
}
}  // namespace quota
