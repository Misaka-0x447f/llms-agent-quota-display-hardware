#include <unity.h>
#include <initializer_list>
#include "../../src/row_view.h"

using quota::UiState;

void setUp() {}
void tearDown() {}

// 空闲、刷新中、未配置都没有需要告知用户的内容，不算异常。
void idle_states_do_not_count_as_abnormal() {
  const UiState idle[] = {UiState::kReady, UiState::kLoading, UiState::kConnecting,
                          UiState::kUnconfigured};
  for (UiState state : idle) {
    TEST_ASSERT_FALSE(quota::abnormal(state));
  }
}

// 用户举例的三类失败（登录失效、网络错误、请求超时）必须算异常。
void failure_states_count_as_abnormal() {
  const UiState failures[] = {
      UiState::kConfigError,     UiState::kWifiNoNetwork,
      UiState::kWifiConnectionFailed, UiState::kWifiTimeout,
      UiState::kWifiDisconnected, UiState::kDnsError,
      UiState::kTlsError,        UiState::kTcpRefused,
      UiState::kRequestTimeout,  UiState::kConnectionLost,
      UiState::kTransportError,  UiState::kAuthError,
      UiState::kRateLimitError,  UiState::kServerError,
      UiState::kHttpError,       UiState::kDataError,
  };
  for (UiState state : failures) {
    TEST_ASSERT_TRUE(quota::abnormal(state));
  }
}

void error_states_have_a_readable_status_word() {
  TEST_ASSERT_EQUAL_STRING("登录失效", quota::uiMessage(UiState::kAuthError));
  TEST_ASSERT_EQUAL_STRING("请求超时", quota::uiMessage(UiState::kRequestTimeout));
  TEST_ASSERT_EQUAL_STRING("", quota::uiMessage(UiState::kReady));
}

// 相位按 2 秒一拍交替，且只取决于时间：同一时刻所有行拿到同一个相位。
void phase_alternates_and_is_shared() {
  TEST_ASSERT_EQUAL_UINT8(1, quota::viewPhase(0));
  TEST_ASSERT_EQUAL_UINT8(1, quota::viewPhase(quota::kViewHoldMs - 1));
  TEST_ASSERT_EQUAL_UINT8(2, quota::viewPhase(quota::kViewHoldMs));
  TEST_ASSERT_EQUAL_UINT8(2, quota::viewPhase(2 * quota::kViewHoldMs - 1));
  TEST_ASSERT_EQUAL_UINT8(1, quota::viewPhase(2 * quota::kViewHoldMs));
  // 相位只取决于时间：相邻两个相位的取值正好相反，因此同一时刻所有行一致。
  for (uint32_t now = 0; now < 10 * quota::kViewHoldMs; now += 1000) {
    const uint8_t phase = quota::viewPhase(now);
    TEST_ASSERT_TRUE(phase == 1 || phase == 2);
    const uint8_t next = quota::viewPhase(now + quota::kViewHoldMs);
    TEST_ASSERT_EQUAL_UINT8(phase == 1 ? 2 : 1, next);
  }
}

// 产品规则：每个视图停留 2 秒（用户 2026-11-13 确认；实现曾用 4 秒）。
void view_hold_is_two_seconds() { TEST_ASSERT_EQUAL_UINT32(2000, quota::kViewHoldMs); }

// 有数据时：只在异常行且相位为 2 才切视图 2；没有异常时两个视图完全相同。
void status_view_requires_an_error_in_phase_two() {
  TEST_ASSERT_FALSE(quota::showStatusView(false, true, 1));
  TEST_ASSERT_FALSE(quota::showStatusView(false, true, 2));
  TEST_ASSERT_FALSE(quota::showStatusView(true, true, 1));
  TEST_ASSERT_TRUE(quota::showStatusView(true, true, 2));
}

// 异常行且还没有任何数据时：视图 1 是空的，必须常驻视图 2，
// 否则整行会在「空白」和「状态词」之间闪，看起来像故障。
void dataless_error_row_never_blinks() {
  TEST_ASSERT_TRUE(quota::showStatusView(true, false, 1));
  TEST_ASSERT_TRUE(quota::showStatusView(true, false, 2));
  // 没有异常时仍然显示视图 1（此时是占位符 + loading 线）。
  TEST_ASSERT_FALSE(quota::showStatusView(false, false, 1));
  TEST_ASSERT_FALSE(quota::showStatusView(false, false, 2));
}

// 视图 2 的横向排版：任何宽度组合下元素都不重叠、不越过 128px。
void status_line_layout_never_overlaps() {
  const int16_t width = 128;
  struct Case {
    int16_t state;
    int16_t label;
    int16_t age;
    bool stale;
    bool expectLabel;
    bool expectStar;
    bool expectAge;
  };
  const Case cases[] = {
      // "deepseek" + 4 + "*" + "<50s"：40 + 6 + 24 = 70 ≤ 78，全部放得下
      {48, 36, 24, true, true, true, true},
      // 平台名 48：52 + 6 + 24 = 82 > 78，平台名让位，* 与过期时间保留
      {48, 48, 24, true, false, true, true},
      // 状态词更宽：只留 * 与过期时间
      {72, 96, 24, true, false, true, true},
      // 没有陈旧标记时只剩过期时间
      {48, 96, 24, false, false, false, true},
      // 极端状态词：整行让给状态词，不再画其它元素
      {126, 48, 24, true, false, false, false},
  };
  for (const Case& c : cases) {
    const quota::StatusLineLayout layout =
        quota::statusLineLayout(width, c.state, c.label, c.age, c.stale);
    TEST_ASSERT_EQUAL(c.expectLabel, layout.showLabel);
    TEST_ASSERT_EQUAL(c.expectStar, layout.showStar);
    TEST_ASSERT_EQUAL(c.expectAge, layout.showAge);
    // 状态词自身不越界（stateWidth 恰好等于 width 时允许贴左）
    TEST_ASSERT_TRUE(layout.stateX + c.state <= width);
    TEST_ASSERT_TRUE(layout.ageX >= 0 && layout.ageX <= width);
    // 左侧内容不得侵入状态词左侧 2px 间隙
    const int16_t leftEdge = layout.ageX + (layout.showAge ? c.age : 0);
    if (layout.showAge) {
      TEST_ASSERT_TRUE(leftEdge <= layout.stateX - quota::kStatusStateGap);
    }
    if (layout.showStar) {
      TEST_ASSERT_EQUAL_INT16(layout.ageX - quota::kStatusStarWidth, layout.starX);
    }
  }
}

// 界面文案的显示宽度上限：状态词太长会把过期时间挤掉，改文案时要被这里拦住。
// 估算与 drawUiText 一致：ASCII 6px，中文 12px。
int16_t estimatedWidth(const char* text) {
  int16_t width = 0;
  while (*text != '\0') {
    const uint8_t first = static_cast<uint8_t>(*text);
    const int length = first < 0x80 ? 1 : ((first & 0xe0) == 0xc0 ? 2 : 3);
    width += length == 1 ? 6 : 12;
    text += length;
  }
  return width;
}

void status_words_stay_within_the_reserved_width() {
  const quota::UiState all[] = {
      quota::UiState::kUnconfigured,     quota::UiState::kConfigError,
      quota::UiState::kConnecting,       quota::UiState::kLoading,
      quota::UiState::kReady,            quota::UiState::kWifiNoNetwork,
      quota::UiState::kWifiConnectionFailed, quota::UiState::kWifiTimeout,
      quota::UiState::kWifiDisconnected, quota::UiState::kDnsError,
      quota::UiState::kTlsError,         quota::UiState::kTcpRefused,
      quota::UiState::kRequestTimeout,   quota::UiState::kConnectionLost,
      quota::UiState::kTransportError,   quota::UiState::kAuthError,
      quota::UiState::kRateLimitError,   quota::UiState::kServerError,
      quota::UiState::kHttpError,        quota::UiState::kDataError,
  };
  // 128px 屏幕：状态词最多占 84px，剩下 44px 给「* + 过期时间」。一个放了一周
  // 的余额是 10080m（36px），仍然放得下。
  const int16_t maxStateWidth = 84;
  for (quota::UiState state : all) {
    const int16_t width = estimatedWidth(quota::uiMessage(state));
    TEST_ASSERT_TRUE(width <= maxStateWidth);
    if (width == 0) continue;
    const quota::StatusLineLayout layout =
        quota::statusLineLayout(128, width, estimatedWidth("openrouter"), 36, true);
    TEST_ASSERT_EQUAL_INT16(128 - width, layout.stateX);
    TEST_ASSERT_TRUE(layout.showAge);
  }
}

// 状态词本身永远完整：即使宽到 126px 也只让别的元素让位，不会被截断。
void widest_status_word_still_fits_the_row() {
  const int16_t width = 128;
  const int16_t widest = 84;  // uiMessage() 里最长的中文状态词约 72～84px
  const quota::StatusLineLayout layout = quota::statusLineLayout(width, widest, 48, 36, true);
  TEST_ASSERT_EQUAL_INT16(width - widest, layout.stateX);
  TEST_ASSERT_TRUE(layout.stateX > 0);
}

// loading 进度条：一段 32px 的高光从屏幕左侧外面扫到右侧外面，2.4 秒一轮。
void loading_bar_sweeps_across_and_wraps() {
  const int16_t width = 128;
  TEST_ASSERT_EQUAL_INT16(-quota::kLoadingBarWidth, quota::loadingBarSpan(0, width).x);
  TEST_ASSERT_EQUAL_INT16(quota::kLoadingBarWidth, quota::loadingBarSpan(0, width).width);
  // 半个周期时高光正好走到屏幕中间附近，且宽度不变。
  const quota::LoadingBarSpan middle =
      quota::loadingBarSpan(quota::kLoadingSweepMs / 2, width);
  TEST_ASSERT_EQUAL_INT16(quota::kLoadingBarWidth, middle.width);
  TEST_ASSERT_TRUE(middle.x > 0);
  TEST_ASSERT_TRUE(middle.x + middle.width < width);
  // 越靠后越靠右，周期结束时回到起点（继续下一轮）。
  int16_t previous = -quota::kLoadingBarWidth;
  for (uint32_t elapsed = 0; elapsed < quota::kLoadingSweepMs; elapsed += 80) {
    const int16_t x = quota::loadingBarSpan(elapsed, width).x;
    TEST_ASSERT_TRUE(x >= previous);
    previous = x;
  }
  TEST_ASSERT_TRUE(previous + quota::kLoadingBarWidth >= width);
  TEST_ASSERT_EQUAL_INT16(quota::loadingBarSpan(0, width).x,
                          quota::loadingBarSpan(quota::kLoadingSweepMs, width).x);
}

// 高光不能因为溢出画到整数范围外面（周期内位置始终落在 [-宽度, 屏幕宽度]）。
void loading_bar_stays_within_the_sweep_range() {
  const int16_t width = 128;
  for (uint32_t elapsed = 0; elapsed < 3 * quota::kLoadingSweepMs; elapsed += 37) {
    const quota::LoadingBarSpan span = quota::loadingBarSpan(elapsed, width);
    TEST_ASSERT_TRUE(span.x >= -quota::kLoadingBarWidth);
    TEST_ASSERT_TRUE(span.x <= width);
  }
}

// 只重发高光扫过的 tile 列：单帧 I²C 量必须远小于整页 16 列。
void loading_bar_flush_covers_old_and_new_positions() {
  const int16_t width = 128;
  const int16_t bar = quota::kLoadingBarWidth;
  // 平稳移动一帧：新旧位置相交，合并成一段，覆盖被擦掉的旧列。
  const quota::LoadingBarFlush middle = quota::loadingBarFlush(40, 48, bar, width);
  TEST_ASSERT_EQUAL_UINT8(1, middle.count);
  TEST_ASSERT_TRUE(middle.spans[0].tw >= 4 && middle.spans[0].tw < 16);
  TEST_ASSERT_TRUE(middle.spans[0].tx * 8 <= 40);
  TEST_ASSERT_TRUE((middle.spans[0].tx + middle.spans[0].tw) * 8 >= 48 + bar);
  // 完全在屏幕左侧外面时不需要刷新任何列。
  TEST_ASSERT_EQUAL_UINT8(0, quota::loadingBarFlush(-bar, -bar, bar, width).count);
  // 刚进入屏幕时从第 0 列开始。
  const quota::LoadingBarFlush enter = quota::loadingBarFlush(-bar, -8, bar, width);
  TEST_ASSERT_EQUAL_UINT8(1, enter.count);
  TEST_ASSERT_EQUAL_UINT8(0, enter.spans[0].tx);
  // 扫到最右侧时不超过屏幕宽度。
  const quota::LoadingBarFlush exitRight = quota::loadingBarFlush(width - 8, width, bar, width);
  TEST_ASSERT_EQUAL_UINT8(1, exitRight.count);
  TEST_ASSERT_TRUE((exitRight.spans[0].tx + exitRight.spans[0].tw) <= 16);
}

// 整段重画（起步/切换/整屏重画后）要一次覆盖新旧两处位置。
void loading_bar_full_flush_covers_both_positions() {
  const int16_t width = 128;
  const int16_t bar = quota::kLoadingBarWidth;
  const quota::LoadingBarFlush full = quota::loadingBarFlush(40, 48, bar, width);
  TEST_ASSERT_EQUAL_UINT8(1, full.count);
  TEST_ASSERT_TRUE(full.spans[0].tx * 8 <= 40);
  TEST_ASSERT_TRUE((full.spans[0].tx + full.spans[0].tw) * 8 >= 48 + bar);
}

// 增量刷新只发两端变化：中间没变的部分不重发。
void loading_bar_diff_only_flushes_the_changed_edges() {
  const int16_t width = 128;
  const int16_t bar = quota::kLoadingBarWidth;
  // 向右移动 4px：左端擦 [40,44)、右端补 [76,80)，各 1 个 tile。
  const quota::LoadingBarFlush right = quota::loadingBarFlushDiff(40, 44, bar, width);
  TEST_ASSERT_EQUAL_UINT8(2, right.count);
  TEST_ASSERT_EQUAL_UINT8(5, right.spans[0].tx);
  TEST_ASSERT_EQUAL_UINT8(1, right.spans[0].tw);
  TEST_ASSERT_EQUAL_UINT8(9, right.spans[1].tx);
  TEST_ASSERT_EQUAL_UINT8(1, right.spans[1].tw);
  // 比整段重画少得多：整段是 1 段 5 个 tile。
  const quota::LoadingBarFlush full = quota::loadingBarFlush(40, 44, bar, width);
  TEST_ASSERT_EQUAL_UINT8(1, full.count);
  TEST_ASSERT_TRUE(full.spans[0].tw > right.spans[0].tw + right.spans[1].tw - 1);
  // 向左移动：两端同样各一小段。
  const quota::LoadingBarFlush left = quota::loadingBarFlushDiff(44, 40, bar, width);
  TEST_ASSERT_EQUAL_UINT8(2, left.count);
  TEST_ASSERT_EQUAL_UINT8(5, left.spans[0].tx);
  TEST_ASSERT_EQUAL_UINT8(9, left.spans[1].tx);
  // 没有移动：什么都不用发。
  TEST_ASSERT_EQUAL_UINT8(0, quota::loadingBarFlushDiff(40, 40, bar, width).count);
  // 回卷（左边刚进场、右边刚出屏）是两小段，不是整屏。
  const quota::LoadingBarFlush wrap = quota::loadingBarFlushDiff(124, -bar + 4, bar, width);
  TEST_ASSERT_EQUAL_UINT8(2, wrap.count);
  TEST_ASSERT_EQUAL_UINT8(15, wrap.spans[0].tx);
  TEST_ASSERT_EQUAL_UINT8(0, wrap.spans[1].tx);
  TEST_ASSERT_TRUE(wrap.spans[0].tw + wrap.spans[1].tw < 4);
}

// 高光完全在屏幕右侧外面时同样不需要刷新（只有旧的左段要擦）。
void loading_bar_flush_handles_parts_off_screen() {
  const int16_t width = 128;
  const int16_t bar = quota::kLoadingBarWidth;
  // 从屏幕内移出右边界：只保留左侧那段。
  const quota::LoadingBarFlush leaving = quota::loadingBarFlush(width - 4, width + 8, bar, width);
  TEST_ASSERT_EQUAL_UINT8(1, leaving.count);
  TEST_ASSERT_EQUAL_UINT8(15, leaving.spans[0].tx);
  // 完全在右侧外面：一个段都不需要。
  TEST_ASSERT_EQUAL_UINT8(0, quota::loadingBarFlush(width + 1, width + 9, bar, width).count);
}

// 窗口行排版：徽标占 13px，重置时间随之右移；配速不压到百分比上。
void window_row_layout_reserves_space_for_the_badge() {
  quota::WindowRowLayout layout = quota::windowRowLayout(
      true, 28, 21, 21, 0, false);  // [3d] + 95% + "-4h"
  TEST_ASSERT_TRUE(layout.showBadge);
  TEST_ASSERT_EQUAL_INT16(0, layout.badgeX);
  // 徽标占 13px（9px 块 + 4px 间隔），块高 12px（向上给字母补 1px 白边）：
  // 这些都是用户可见的固定值，不只是实现细节。
  TEST_ASSERT_EQUAL_INT16(13, quota::kBadgeAdvance);
  TEST_ASSERT_EQUAL_INT16(9, quota::kBadgeWidth);
  TEST_ASSERT_EQUAL_INT16(12, quota::kBadgeHeight);
  TEST_ASSERT_EQUAL_INT16(13, layout.resetX);
  TEST_ASSERT_TRUE(layout.showDelta);
  TEST_ASSERT_TRUE(layout.showPercent);
  TEST_ASSERT_EQUAL_INT16(128 - 21, layout.percentX);
  // Δ 与配速都在重置时间之后、百分比之前，不重叠。
  TEST_ASSERT_TRUE(layout.deltaGlyphX > layout.resetX);
  TEST_ASSERT_TRUE(layout.deltaTextX > layout.deltaGlyphX);
  TEST_ASSERT_TRUE(layout.deltaTextX + 21 <= layout.percentX - 1);
}

// 没有徽标时和改动前完全一致：重置时间在 x=0。
void window_row_layout_without_badge_starts_at_the_edge() {
  quota::WindowRowLayout layout = quota::windowRowLayout(false, 28, 21, 21, 0, false);
  TEST_ASSERT_FALSE(layout.showBadge);
  TEST_ASSERT_EQUAL_INT16(0, layout.resetX);
  TEST_ASSERT_TRUE(layout.showDelta);
}

// 配速太长时整组让位，而不是压到百分比上。
void window_row_layout_drops_the_delta_instead_of_overlapping() {
  quota::WindowRowLayout layout = quota::windowRowLayout(true, 28, 28, 56, 0, false);
  TEST_ASSERT_FALSE(layout.showDelta);
  TEST_ASSERT_TRUE(layout.showPercent);
  TEST_ASSERT_EQUAL_INT16(128 - 28, layout.percentX);
}

// 充值形态：百分比紧跟重置时间（空一格），金额贴右，不画配速。
void credit_mode_puts_the_amount_on_the_right() {
  quota::WindowRowLayout layout = quota::windowRowLayout(true, 28, 14, 0, 35, true);
  TEST_ASSERT_TRUE(layout.showPercent);
  TEST_ASSERT_TRUE(layout.showAmount);
  TEST_ASSERT_FALSE(layout.showDelta);
  TEST_ASSERT_EQUAL_INT16(quota::kBadgeAdvance + 28 + quota::kWindowCharWidth,
                          layout.percentX);
  TEST_ASSERT_EQUAL_INT16(128 - 35, layout.amountX);
  // 百分比与金额之间至少留 kCreditGap。
  TEST_ASSERT_TRUE(layout.percentX + 14 + quota::kCreditGap <= layout.amountX);
}

// 金额很长时先去掉空格，再去掉百分比，金额本身始终保留。
void credit_mode_degrades_before_dropping_the_amount() {
  quota::WindowRowLayout tight =
      quota::windowRowLayout(true, 28, 14, 0, 60, true);
  TEST_ASSERT_TRUE(tight.showPercent == false ||
                   tight.percentX + 14 + quota::kCreditGap <= tight.amountX);
  TEST_ASSERT_TRUE(tight.showAmount);
  TEST_ASSERT_EQUAL_INT16(128 - 60, tight.amountX);

  quota::WindowRowLayout noPercent = quota::windowRowLayout(true, 28, 0, 0, 35, true);
  TEST_ASSERT_FALSE(noPercent.showPercent);
  TEST_ASSERT_TRUE(noPercent.showAmount);
}

// 极端宽的金额（远超实际量级）连重置时间都放不下时，明确让掉重置时间，
// 只保留徽标与金额，而不是留一片空白。
void credit_mode_keeps_the_amount_when_it_cannot_fit_with_the_reset() {
  quota::WindowRowLayout layout = quota::windowRowLayout(true, 28, 14, 0, 100, true);
  TEST_ASSERT_TRUE(layout.showAmount);
  TEST_ASSERT_EQUAL_INT16(128 - 100, layout.amountX);
  TEST_ASSERT_FALSE(layout.showReset);
  TEST_ASSERT_FALSE(layout.showPercent);
  TEST_ASSERT_TRUE(layout.showBadge);

  // 实际量级（最多约 8 个字符）三者都显示。
  quota::WindowRowLayout normal = quota::windowRowLayout(true, 28, 14, 0, 56, true);
  TEST_ASSERT_TRUE(normal.showReset);
  TEST_ASSERT_TRUE(normal.showPercent);
  TEST_ASSERT_TRUE(normal.showAmount);
}

// 两个降级分支的边界：金额 65px 时百分比仍放得下（去掉空格），72px 时才让掉百分比。
void credit_mode_degradation_boundaries_are_explicit() {
  // reset 28 + 13 + 7 + percent 14 = 62，金额 65 → 起点 63，留不出 2px：
  // 去掉「重置时间与百分比之间的空格」，百分比贴着重置时间。
  quota::WindowRowLayout noSpace = quota::windowRowLayout(true, 28, 14, 0, 65, true);
  TEST_ASSERT_TRUE(noSpace.showReset);
  TEST_ASSERT_TRUE(noSpace.showPercent);
  TEST_ASSERT_TRUE(noSpace.showAmount);
  TEST_ASSERT_EQUAL_INT16(quota::kBadgeAdvance + 28, noSpace.percentX);
  TEST_ASSERT_TRUE(noSpace.percentX + 14 + quota::kCreditGap <= noSpace.amountX);

  // 72px：即使去掉空格也放不下百分比，只保留金额。
  quota::WindowRowLayout noPercent = quota::windowRowLayout(true, 28, 14, 0, 72, true);
  TEST_ASSERT_TRUE(noPercent.showReset);
  TEST_ASSERT_FALSE(noPercent.showPercent);
  TEST_ASSERT_TRUE(noPercent.showAmount);
  TEST_ASSERT_EQUAL_INT16(128 - 72, noPercent.amountX);
}

// 金额宽到会压住徽标时徽标让位；比整行还宽时明确不画金额。
void credit_mode_gives_the_badge_way_to_a_very_wide_amount() {
  // 119px：金额可独占整行，重置时间与徽标都让位。
  quota::WindowRowLayout wide = quota::windowRowLayout(true, 28, 14, 0, 119, true);
  TEST_ASSERT_TRUE(wide.showAmount);
  TEST_ASSERT_FALSE(wide.showReset);
  TEST_ASSERT_FALSE(wide.showBadge);
  TEST_ASSERT_EQUAL_INT16(128 - 119, wide.amountX);

  // 126px：金额起点 2，已压到徽标，徽标必须让位。
  quota::WindowRowLayout wider = quota::windowRowLayout(true, 28, 14, 0, 126, true);
  TEST_ASSERT_TRUE(wider.showAmount);
  TEST_ASSERT_FALSE(wider.showBadge);
  TEST_ASSERT_TRUE(wider.amountX >= 0);

  // 128px：正好占满整行。
  quota::WindowRowLayout exact = quota::windowRowLayout(true, 28, 14, 0, 128, true);
  TEST_ASSERT_TRUE(exact.showAmount);
  TEST_ASSERT_EQUAL_INT16(0, exact.amountX);

  // 129px：放不下，明确不画。
  quota::WindowRowLayout overflow = quota::windowRowLayout(true, 28, 14, 0, 129, true);
  TEST_ASSERT_FALSE(overflow.showAmount);
}

// 所有形态都不越出 128px：元素右边缘（含留白）应落在屏幕内。
void window_row_layout_stays_on_screen() {
  const int16_t resetWidths[] = {28, 35, 42};   // [3d] [12h] [123m]
  const int16_t percents[] = {14, 21, 28};      // 9% 95% 100%
  const int16_t deltas[] = {0, 14, 21, 35};     // -- -4h -4h -12h3m
  const int16_t amounts[] = {0, 21, 35, 42};    // 0.00 12.34 1234.56
  for (bool badge : {false, true}) {
    for (int16_t reset : resetWidths) {
      for (int16_t percent : percents) {
        for (int16_t delta : deltas) {
          quota::WindowRowLayout layout =
              quota::windowRowLayout(badge, reset, percent, delta, 0, false);
          TEST_ASSERT_TRUE(layout.resetX + reset <= 128);
          if (layout.showPercent) {
            TEST_ASSERT_TRUE(layout.percentX + percent <= 128);
          }
          if (layout.showDelta) {
            // 实现保证配速与百分比之间至少留 1px，这里按同样的口径断言。
            TEST_ASSERT_TRUE(layout.deltaTextX + delta <= layout.percentX - 1);
          }
        }
        for (int16_t amount : amounts) {
          quota::WindowRowLayout layout =
              quota::windowRowLayout(badge, reset, percent, 0, amount, true);
          if (layout.showAmount) {
            TEST_ASSERT_TRUE(layout.amountX + amount <= 128);
            TEST_ASSERT_TRUE(layout.amountX >= 0);
          }
          if (layout.showPercent && layout.showAmount) {
            TEST_ASSERT_TRUE(layout.percentX + percent <= layout.amountX);
          }
        }
      }
    }
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(idle_states_do_not_count_as_abnormal);
  RUN_TEST(failure_states_count_as_abnormal);
  RUN_TEST(error_states_have_a_readable_status_word);
  RUN_TEST(phase_alternates_and_is_shared);
  RUN_TEST(view_hold_is_two_seconds);
  RUN_TEST(status_view_requires_an_error_in_phase_two);
  RUN_TEST(dataless_error_row_never_blinks);
  RUN_TEST(status_line_layout_never_overlaps);
  RUN_TEST(widest_status_word_still_fits_the_row);
  RUN_TEST(status_words_stay_within_the_reserved_width);
  RUN_TEST(loading_bar_sweeps_across_and_wraps);
  RUN_TEST(loading_bar_stays_within_the_sweep_range);
  RUN_TEST(loading_bar_flush_covers_old_and_new_positions);
  RUN_TEST(loading_bar_full_flush_covers_both_positions);
  RUN_TEST(loading_bar_flush_handles_parts_off_screen);
  RUN_TEST(loading_bar_diff_only_flushes_the_changed_edges);
  RUN_TEST(window_row_layout_reserves_space_for_the_badge);
  RUN_TEST(window_row_layout_without_badge_starts_at_the_edge);
  RUN_TEST(window_row_layout_drops_the_delta_instead_of_overlapping);
  RUN_TEST(credit_mode_puts_the_amount_on_the_right);
  RUN_TEST(credit_mode_degrades_before_dropping_the_amount);
  RUN_TEST(credit_mode_keeps_the_amount_when_it_cannot_fit_with_the_reset);
  RUN_TEST(credit_mode_gives_the_badge_way_to_a_very_wide_amount);
  RUN_TEST(credit_mode_degradation_boundaries_are_explicit);
  RUN_TEST(window_row_layout_stays_on_screen);
  return UNITY_END();
}
