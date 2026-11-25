#pragma once

#ifdef ARDUINO
#include <U8g2lib.h>  // 固件：Arduino 封装，内部含 clib/u8g2.h 的 C API
#else
#include <u8g2.h>     // 电脑端视觉验证脚本：直接用 C API
#endif

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "delta_format.h"
#include "row_view.h"

// 窗口行（视图 1）的完整绘制：重置倒计时、配速 Δ、剩余百分比，以及
// 2026-11-25 新增的两件事——左侧反色徽标，和额度用尽后改为显示充值余额。
//
// 用 u8g2 的 C API + `u8g2_t*` 而不是 Arduino 的 `U8G2&`，这样同一份代码
// 既能烧进固件（`display.getU8g2()`），也能在电脑上渲染成图片做视觉验证。
namespace window_render {

constexpr uint32_t kFiveHourSeconds = 5 * 3600;

struct Input {
  bool present = false;
  uint32_t resetAt = 0;         // Unix 秒；0 表示未知
  uint32_t durationSeconds = 0;  // 窗口长度（5h / 7d）
  float usedPercent = 0;
  uint32_t nowEpoch = 0;         // 0 表示时钟还没同步
  const char* badge = nullptr;   // 单个字母；nullptr 或空串表示不画
  // 充值形态：不画配速，右侧显示余额（credits ÷ 100）。
  bool creditMode = false;
  bool creditsKnown = false;
  double creditAmount = 0;
};

// "3d" / "12h" / "45m"；extended 时带次级单位（例如 2d3h）。
inline void formatDurationText(uint32_t seconds, bool extended, char* out, size_t size) {
  struct Unit {
    uint32_t seconds;
    char suffix;
  };
  static const Unit units[] = {{86400, 'd'}, {3600, 'h'}, {60, 'm'}};
  for (size_t i = 0; i < 3; ++i) {
    if (seconds >= units[i].seconds || i == 2) {
      const int written = snprintf(out, size, "%u%c",
                                   static_cast<unsigned>(seconds / units[i].seconds),
                                   units[i].suffix);
      if (extended && i + 1 < 3 && written > 0 && static_cast<size_t>(written) < size) {
        const uint32_t minor = (seconds % units[i].seconds) / units[i + 1].seconds;
        if (minor != 0) {
          snprintf(out + written, size - static_cast<size_t>(written), "%u%c",
                   static_cast<unsigned>(minor), units[i + 1].suffix);
        }
      }
      return;
    }
  }
  snprintf(out, size, "0m");
}

// 反色小徽标：白底实心块 + 用背景色擦出来的字母。
// 方块 9×12：比 7×14 的字格宽 2px，顶部比大写字母墨迹再高 1px，
// 让字母上下各留 1px 白边（横向约 2px）。
inline void drawBadge(u8g2_t* u8g2, int16_t x, int16_t baseline, const char* letter,
                      const uint8_t* font) {
  u8g2_SetDrawColor(u8g2, 1);
  u8g2_DrawBox(u8g2, x, static_cast<u8g2_uint_t>(baseline - 11), quota::kBadgeWidth,
               quota::kBadgeHeight);
  u8g2_SetDrawColor(u8g2, 0);
  u8g2_SetFont(u8g2, font);
  u8g2_DrawStr(u8g2, static_cast<u8g2_uint_t>(x + 1), static_cast<u8g2_uint_t>(baseline),
               letter);
  u8g2_SetDrawColor(u8g2, 1);
}

inline void drawWindowRow(u8g2_t* u8g2, int16_t y, const Input& in, const uint8_t* font) {
  u8g2_SetFont(u8g2, font);
  const bool hasBadge = in.badge != nullptr && in.badge[0] != '\0';
  // 没有数据时也占住这一行：留一片空白会让该行的 loading 线看起来属于下一行。
  // 余额行的 `--` 就是这样处理的，这里保持一致。
  if (!in.present) {
    if (hasBadge) drawBadge(u8g2, 0, y, in.badge, font);
    const int16_t resetX = hasBadge ? quota::kBadgeAdvance : 0;
    u8g2_DrawStr(u8g2, static_cast<u8g2_uint_t>(resetX), static_cast<u8g2_uint_t>(y), "[--]");
    const int16_t dashX = static_cast<int16_t>(quota::kRowWidth - u8g2_GetStrWidth(u8g2, "--"));
    u8g2_DrawStr(u8g2, static_cast<u8g2_uint_t>(dashX), static_cast<u8g2_uint_t>(y), "--");
    return;
  }
  const uint32_t remainSeconds = in.resetAt > in.nowEpoch ? in.resetAt - in.nowEpoch : 0;
  const float remaining = 100.0f - in.usedPercent;
  // 时钟未同步时 resetAt 无法换算成剩余时间，显示 -- 而不是把时间戳当天数。
  char label[16];
  if (in.resetAt == 0) {
    snprintf(label, sizeof(label), "%s",
             in.durationSeconds == kFiveHourSeconds ? "5h" : "7d");
  } else if (in.nowEpoch == 0) {
    snprintf(label, sizeof(label), "--");
  } else {
    formatDurationText(remainSeconds, false, label, sizeof(label));
  }
  char percent[8];
  snprintf(percent, sizeof(percent), "%d%%", static_cast<int>(roundf(remaining)));
  char reset[24];
  snprintf(reset, sizeof(reset), "[%s]", label);

  // 充值形态：金额（credits ÷ 100）是信息主体，配速没有意义，不画 Δ。
  char amount[32];
  snprintf(amount, sizeof(amount), "%.2f", in.creditAmount);
  // 金额比整行还宽（实际量级不会出现）：用 `--` 占位，保住这一行的身份，
  // 不要为了让金额显示而把倒计时和徽标都让位、最后只留一片空白。
  if (u8g2_GetStrWidth(u8g2, amount) > quota::kRowWidth) {
    snprintf(amount, sizeof(amount), "--");
  }
  char delta[16] = {};
  bool hasDelta = false;
  if (!in.creditMode) {
    if (in.resetAt == 0 || in.durationSeconds == 0 || in.nowEpoch == 0) {
      snprintf(delta, sizeof(delta), "--");
    } else {
      const float expectedRemaining =
          remaining * static_cast<float>(in.durationSeconds) / 100.0f;
      const float rawDelta = static_cast<float>(remainSeconds) - expectedRemaining;
      if (!quota::formatMinuteDelta(-rawDelta, delta, sizeof(delta))) {
        const char sign = rawDelta >= 0 ? '-' : '+';
        const uint32_t magnitude = static_cast<uint32_t>(fabsf(rawDelta));
        char span[12];
        formatDurationText(magnitude, magnitude >= 86400, span, sizeof(span));
        snprintf(delta, sizeof(delta), "%c%s", sign, span);
      }
    }
    hasDelta = true;
  }

  const quota::WindowRowLayout layout = quota::windowRowLayout(
      hasBadge, u8g2_GetStrWidth(u8g2, reset), u8g2_GetStrWidth(u8g2, percent),
      hasDelta ? u8g2_GetStrWidth(u8g2, delta) : 0,
      in.creditMode ? u8g2_GetStrWidth(u8g2, in.creditsKnown ? amount : "--") : 0,
      in.creditMode);
  if (layout.showBadge) drawBadge(u8g2, layout.badgeX, y, in.badge, font);
  if (layout.showReset) {
    u8g2_DrawStr(u8g2, static_cast<u8g2_uint_t>(layout.resetX), static_cast<u8g2_uint_t>(y),
                 reset);
  }
  if (in.creditMode) {
    if (layout.showPercent) {
      u8g2_DrawStr(u8g2, static_cast<u8g2_uint_t>(layout.percentX),
                   static_cast<u8g2_uint_t>(y), percent);
    }
    if (layout.showAmount) {
      u8g2_DrawStr(u8g2, static_cast<u8g2_uint_t>(layout.amountX), static_cast<u8g2_uint_t>(y),
                   in.creditsKnown ? amount : "--");
    }
    return;
  }
  if (layout.showDelta) {
    // 细线框 Δ：避免实心三角形带来的粗重观感。
    const int16_t x = layout.deltaGlyphX;
    u8g2_DrawLine(u8g2, static_cast<u8g2_uint_t>(x + 3),
                  static_cast<u8g2_uint_t>(y - 7), static_cast<u8g2_uint_t>(x),
                  static_cast<u8g2_uint_t>(y));
    u8g2_DrawLine(u8g2, static_cast<u8g2_uint_t>(x), static_cast<u8g2_uint_t>(y),
                  static_cast<u8g2_uint_t>(x + 6), static_cast<u8g2_uint_t>(y));
    u8g2_DrawLine(u8g2, static_cast<u8g2_uint_t>(x + 6), static_cast<u8g2_uint_t>(y),
                  static_cast<u8g2_uint_t>(x + 3), static_cast<u8g2_uint_t>(y - 7));
    u8g2_DrawStr(u8g2, static_cast<u8g2_uint_t>(layout.deltaTextX),
                 static_cast<u8g2_uint_t>(y), delta);
  }
  if (layout.showPercent) {
    u8g2_DrawStr(u8g2, static_cast<u8g2_uint_t>(layout.percentX),
                 static_cast<u8g2_uint_t>(y), percent);
  }
}
}  // namespace window_render
