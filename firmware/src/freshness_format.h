#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

namespace quota {
// 过期时间：距该行上次成功获取的时长。
// 不超过一个刷新间隔时，精确到分钟没有信息量，直接显示「< 间隔」：
// Claude 的 10 分钟间隔显示 <10m，余额的 50 秒间隔折算成 1 分钟显示 <1m。
inline bool ageText(unsigned long elapsedSeconds, unsigned long intervalSeconds,
                    char* out, size_t size) {
  if (elapsedSeconds < intervalSeconds) {
    unsigned long minutes = intervalSeconds / 60;
    if (minutes == 0) minutes = 1;
    return snprintf(out, size, "<%lum", minutes) > 0;
  }
  return snprintf(out, size, "%lum", elapsedSeconds / 60) > 0;
}

// lastOkMs 为 0 表示这一行还没有成功记录，过期时间无从计算，显示 --。
// 用无符号差值算时长，可自然处理 millis 回绕。
inline bool ageSince(uint32_t lastOkMs, uint32_t nowMs, unsigned long intervalSeconds,
                     char* out, size_t size) {
  if (lastOkMs == 0) return snprintf(out, size, "--") > 0;
  return ageText(static_cast<unsigned long>(nowMs - lastOkMs) / 1000UL, intervalSeconds,
                 out, size);
}
}  // namespace quota
