#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

namespace quota {
// 过期时间：距该行上次成功获取的时长。
// 不超过一个刷新间隔时，精确到分钟没有信息量，直接显示「< 间隔」：
// 10 分钟间隔显示 <10m；间隔本身不到 1 分钟时改用秒，避免出现 <1m / 0m 这种
// 掩盖真实新鲜度的读数（余额 50 秒间隔显示 <50s，55 秒时显示 55s）。
inline bool ageText(unsigned long elapsedSeconds, unsigned long intervalSeconds,
                    char* out, size_t size) {
  if (intervalSeconds < 60) {
    if (elapsedSeconds < intervalSeconds) {
      return snprintf(out, size, "<%lus", intervalSeconds) > 0;
    }
    return snprintf(out, size, "%lus", elapsedSeconds) > 0;
  }
  // 选一个能整除间隔的最大单位：600→m、3600→h、86400→d；90 秒这种不能整除
  // 的一律退回秒，避免把 90 秒写成 <1m 这种不成立的阈值。
  unsigned long unit = 1;
  char suffix = 's';
  if (intervalSeconds >= 86400 && intervalSeconds % 86400 == 0) {
    unit = 86400;
    suffix = 'd';
  } else if (intervalSeconds >= 3600 && intervalSeconds % 3600 == 0) {
    unit = 3600;
    suffix = 'h';
  } else if (intervalSeconds >= 60 && intervalSeconds % 60 == 0) {
    unit = 60;
    suffix = 'm';
  }
  if (elapsedSeconds < intervalSeconds) {
    return snprintf(out, size, "<%lu%c", intervalSeconds / unit, suffix) > 0;
  }
  return snprintf(out, size, "%lu%c", elapsedSeconds / unit, suffix) > 0;
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
