// 电脑端渲染窗口行：用固件里同一份 window_render.h 把 128×64 画面画进内存缓冲区，
// 再输出 PBM。用于在没有实物设备（或设备没连）时做视觉验证：屏幕上的每个像素都来自
// 烧进固件的绘制代码和同一套 U8g2 字体。
//
// 构建（在仓库根目录）：
//   gcc -std=c11 -I firmware/.pio/libdeps/esp32c3_supermini/U8g2/src/clib \
//       -o /tmp/render_rows firmware/scripts/render_rows_host.cpp \
//       firmware/.pio/libdeps/esp32c3_supermini/U8g2/src/clib/*.c -lm
//
// 用法：
//   /tmp/render_rows <输出目录>
// 每个场景输出一个 .pbm（128×64，1 = 白）。
#include <stdio.h>
#include <string.h>

#include "antigravity_protocol.h"
#include "row_config.h"
#include "window_render.h"

namespace {

constexpr int kWidth = 128;
constexpr int kHeight = 64;

// u8g2 自己管理整屏缓冲（_f 模式）；像素从 u8g2_GetBufferPtr() 读出。
uint8_t* frame(u8g2_t* u8g2) { return u8g2_GetBufferPtr(u8g2); }

void clear(u8g2_t* u8g2) { u8g2_ClearBuffer(u8g2); }

bool framePath(char* out, size_t size, const char* dir, const char* name) {
  const int written = snprintf(out, size, "%s/%s", dir, name);
  if (written < 0 || static_cast<size_t>(written) >= size) {
    fprintf(stderr, "output path too long: %s/%s\n", dir, name);
    return false;
  }
  return true;
}

bool dump(const char* path, u8g2_t* u8g2) {
  const uint8_t* buffer = frame(u8g2);
  FILE* file = fopen(path, "wb");
  if (file == nullptr) {
    fprintf(stderr, "cannot write %s\n", path);
    return false;
  }
  fprintf(file, "P4\n%d %d\n", kWidth, kHeight);
  // u8g2 的缓冲区是 tile 布局：每 8 行一页，页内按列，低位在上。
  uint8_t row[kWidth];
  for (int y = 0; y < kHeight; ++y) {
    const int page = y / 8;
    const int bit = y % 8;
    for (int x = 0; x < kWidth; ++x) {
      const uint8_t byte = buffer[page * kWidth + x];
      row[x] = ((byte >> bit) & 1u) ? 1 : 0;
    }
    for (int x = 0; x < kWidth; x += 8) {
      uint8_t packed = 0;
      for (int b = 0; b < 8; ++b) {
        if (x + b < kWidth) packed |= static_cast<uint8_t>(row[x + b] << (7 - b));
      }
      fputc(packed, file);
    }
  }
  const bool flushed = fflush(file) == 0 && !ferror(file);
  const bool closed = fclose(file) == 0;
  if (!flushed || !closed) {
    fprintf(stderr, "write error for %s\n", path);
    return false;
  }
  return true;
}

void balanceRow(u8g2_t* u8g2, int16_t baseline, const char* label, const char* value) {
  u8g2_SetDrawColor(u8g2, 1);
  u8g2_SetFont(u8g2, u8g2_font_6x12_tf);
  u8g2_DrawStr(u8g2, 0, baseline, label);
  u8g2_SetFont(u8g2, u8g2_font_7x14_tf);
  const int16_t width = static_cast<int16_t>(u8g2_GetStrWidth(u8g2, value));
  u8g2_DrawStr(u8g2, static_cast<u8g2_uint_t>(kWidth - width), baseline, value);
}

// 与 main.cpp 的 drawRow 相同的组合方式。
void windowRow(u8g2_t* u8g2, int16_t baseline, const window_render::Input& input) {
  window_render::drawWindowRow(u8g2, baseline, input, u8g2_font_7x14_tf);
}

// 与实物同一套显示参数（SSD1306/SSD1315 128×64，垂直页布局，整屏缓冲），
// 但输出端用空实现：只画进内存，不发 I²C。
uint8_t g_frame[128 * 64 / 8];

antigravity::RateWindow windowFromUsed(float usedPercent) {
  antigravity::RateWindow window;
  window.present = true;
  window.durationSeconds = antigravity::kSevenDaySeconds;
  window.usedPercent = usedPercent;
  return window;
}

const antigravity::Credits kNoCredits = [] {
  antigravity::Credits credits;
  credits.present = true;
  credits.amount = 0;
  credits.minimum = 0.5;
  return credits;
}();

void setup(u8g2_t* u8g2) {
  u8x8_Setup(u8g2_GetU8x8(u8g2), u8x8_d_ssd1306_128x64_noname, u8x8_cad_001,
             u8x8_byte_empty, u8x8_dummy_cb);
  u8g2_SetupBuffer(u8g2, g_frame, kHeight / 8, u8g2_ll_hvline_vertical_top_lsb,
                   &u8g2_cb_r0);
  u8g2_ClearBuffer(u8g2);
}

}  // namespace

int main(int argc, char** argv) {
  const char* outDir = argc > 1 ? argv[1] : ".";
  char path[512];
  bool ok = true;
  u8g2_t u8g2;
  setup(&u8g2);

  const uint32_t now = 1795608000UL;          // 2026-11-25T12:00:00Z
  const uint32_t reset3d = now + 3 * 86400 + 3 * 3600;   // 约 3 天 3 小时
  const uint32_t reset5d = now + 4 * 86400 + 20 * 3600;  // 约 4 天 20 小时

  // 1. 默认三行：ChatGPT 7d（带 [O]）+ 两个余额。
  clear(&u8g2);
  window_render::Input chatgpt;
  chatgpt.present = true;
  chatgpt.resetAt = reset3d;
  chatgpt.durationSeconds = 604800;
  chatgpt.usedPercent = 60;
  chatgpt.nowEpoch = now;
  chatgpt.badge = "O";
  windowRow(&u8g2, 20, chatgpt);
  balanceRow(&u8g2, 36, "deepseek", "12.34");
  balanceRow(&u8g2, 52, "openrouter", "56.78");
  ok = framePath(path, sizeof(path), outDir, "01-default-three-rows.pbm") &&
       dump(path, &u8g2) && ok;

  // 2. 对照：同样的数据但没有徽标（改动前的版式）。
  clear(&u8g2);
  window_render::Input noBadge = chatgpt;
  noBadge.badge = nullptr;
  windowRow(&u8g2, 20, noBadge);
  balanceRow(&u8g2, 36, "deepseek", "12.34");
  balanceRow(&u8g2, 52, "openrouter", "56.78");
  ok = framePath(path, sizeof(path), outDir, "02-without-badge.pbm") &&
       dump(path, &u8g2) && ok;

  // 3. Antigravity 7d 正常显示：还有 63% 额度。
  clear(&u8g2);
  window_render::Input antigravity;
  antigravity.present = true;
  antigravity.resetAt = reset5d;
  antigravity.durationSeconds = 604800;
  antigravity.usedPercent = 37;
  antigravity.nowEpoch = now;
  antigravity.badge = "G";
  windowRow(&u8g2, 20, antigravity);
  balanceRow(&u8g2, 36, "deepseek", "12.34");
  balanceRow(&u8g2, 52, "openrouter", "56.78");
  ok = framePath(path, sizeof(path), outDir, "03-antigravity-normal.pbm") &&
       dump(path, &u8g2) && ok;

  // 4. 额度用尽，转显示充值余额（12.34 美元）。
  clear(&u8g2);
  window_render::Input overage = antigravity;
  overage.usedPercent = 100;
  overage.creditMode = true;
  overage.creditsKnown = true;
  overage.creditAmount = 12.34;
  windowRow(&u8g2, 20, overage);
  balanceRow(&u8g2, 36, "deepseek", "12.34");
  balanceRow(&u8g2, 52, "openrouter", "56.78");
  ok = framePath(path, sizeof(path), outDir, "04-antigravity-overage.pbm") &&
       dump(path, &u8g2) && ok;

  // 5. 额度用尽且拿不到充值余额：右侧 --。
  clear(&u8g2);
  window_render::Input unknown = overage;
  unknown.creditsKnown = false;
  windowRow(&u8g2, 20, unknown);
  balanceRow(&u8g2, 36, "deepseek", "12.34");
  balanceRow(&u8g2, 52, "openrouter", "56.78");
  ok = framePath(path, sizeof(path), outDir, "05-antigravity-overage-unknown.pbm") &&
       dump(path, &u8g2) && ok;

  // 6. 混合三行：ChatGPT 正常、Antigravity 转余额、一个余额行。
  clear(&u8g2);
  windowRow(&u8g2, 20, chatgpt);
  windowRow(&u8g2, 36, overage);
  balanceRow(&u8g2, 52, "deepseek", "12.34");
  ok = framePath(path, sizeof(path), outDir, "06-mixed-three-rows.pbm") &&
       dump(path, &u8g2) && ok;

  // 7. Claude 两行居中，带 [A]。
  clear(&u8g2);
  window_render::Input claude = chatgpt;
  claude.badge = "A";
  claude.resetAt = now + 6 * 86400 + 2 * 3600;
  claude.usedPercent = 59;
  windowRow(&u8g2, 28, claude);
  balanceRow(&u8g2, 44, "deepseek", "12.34");
  ok = framePath(path, sizeof(path), outDir, "07-claude-two-rows.pbm") &&
       dump(path, &u8g2) && ok;

  // 8. 没有数据：占位符也要带徽标，loading 线位置不变。
  clear(&u8g2);
  window_render::Input empty;
  empty.present = false;
  empty.badge = "G";
  windowRow(&u8g2, 20, empty);
  balanceRow(&u8g2, 36, "deepseek", "12.34");
  balanceRow(&u8g2, 52, "openrouter", "56.78");
  ok = framePath(path, sizeof(path), outDir, "08-no-data-placeholder.pbm") &&
       dump(path, &u8g2) && ok;

  // 9. 四行：基线直接取产品代码的 rowLayout(4)，行距由它决定（14px）。
  quota::RowSlotLayout four[quota::kMaxRows];
  if (quota::rowLayout(4, four)) {
    clear(&u8g2);
    windowRow(&u8g2, four[0].textY, chatgpt);
    windowRow(&u8g2, four[1].textY, antigravity);
    balanceRow(&u8g2, four[2].textY, "deepseek", "12.34");
    balanceRow(&u8g2, four[3].textY, "openrouter", "56.78");
  }
  ok = framePath(path, sizeof(path), outDir, "09-four-rows.pbm") &&
       dump(path, &u8g2) && ok;

  // 10/11. 低余额但周额度仍有剩余：10 = 被撤回的方案（仅作视觉对照，非产品行为），
  // 11 = 现行产品行为（有额度就显示配速；creditMode 由 overageMode 决定）。
  window_render::Input lowBalance = antigravity;
  lowBalance.usedPercent = 27;
  lowBalance.creditMode = true;  // 撤回的方案，仅用于对照
  lowBalance.creditsKnown = true;
  lowBalance.creditAmount = 0;
  clear(&u8g2);
  windowRow(&u8g2, 20, lowBalance);
  balanceRow(&u8g2, 36, "deepseek", "12.34");
  balanceRow(&u8g2, 52, "openrouter", "56.78");
  ok = framePath(path, sizeof(path), outDir, "10-low-balance-switch.pbm") &&
       dump(path, &u8g2) && ok;

  window_render::Input lowBalanceNormal = lowBalance;
  lowBalanceNormal.creditMode = antigravity::overageMode(windowFromUsed(lowBalance.usedPercent),
                                                          kNoCredits);
  if (lowBalanceNormal.creditMode) {
    // 现行规则：73% 剩余不该进入充值形态。
    fprintf(stderr, "overageMode regressed: 73%% remaining switched to credits\n");
    ok = false;
  }
  clear(&u8g2);
  windowRow(&u8g2, 20, lowBalanceNormal);
  balanceRow(&u8g2, 36, "deepseek", "12.34");
  balanceRow(&u8g2, 52, "openrouter", "56.78");
  ok = framePath(path, sizeof(path), outDir, "11-low-balance-normal.pbm") &&
       dump(path, &u8g2) && ok;

  if (!ok) {
    fprintf(stderr, "failed to write every frame (output dir missing?)\n");
    return 1;
  }
  printf("wrote frames to %s\n", outDir);
  return 0;
}
