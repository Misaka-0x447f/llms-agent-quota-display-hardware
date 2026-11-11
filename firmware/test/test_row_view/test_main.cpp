#include <unity.h>
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

// 相位按 4 秒一拍交替，且只取决于时间：同一时刻所有行拿到同一个相位。
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

// 视图 2 只在异常行且相位为 2 时出现；没有异常时两个视图完全相同。
void status_view_requires_an_error_in_phase_two() {
  TEST_ASSERT_FALSE(quota::showStatusView(false, 1));
  TEST_ASSERT_FALSE(quota::showStatusView(false, 2));
  TEST_ASSERT_FALSE(quota::showStatusView(true, 1));
  TEST_ASSERT_TRUE(quota::showStatusView(true, 2));
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
      // deepseek* <1m 请求超时：全部放得下
      {48, 48, 18, true, true, true, true},
      // WiFi无此网络：平台名让位，* 与过期时间保留
      {72, 48, 18, true, false, true, true},
      // 平台名很长且带 *：只留 * 与过期时间
      {72, 96, 24, true, false, true, true},
      // 没有陈旧标记时只剩过期时间
      {48, 96, 24, false, false, false, true},
      // 极端状态词：整行让给状态词，不再画其它元素
      {126, 48, 18, true, false, false, false},
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

int main() {
  UNITY_BEGIN();
  RUN_TEST(idle_states_do_not_count_as_abnormal);
  RUN_TEST(failure_states_count_as_abnormal);
  RUN_TEST(error_states_have_a_readable_status_word);
  RUN_TEST(phase_alternates_and_is_shared);
  RUN_TEST(status_view_requires_an_error_in_phase_two);
  RUN_TEST(status_line_layout_never_overlaps);
  RUN_TEST(widest_status_word_still_fits_the_row);
  RUN_TEST(status_words_stay_within_the_reserved_width);
  return UNITY_END();
}
