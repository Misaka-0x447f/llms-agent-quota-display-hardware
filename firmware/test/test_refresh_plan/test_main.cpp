#include <unity.h>
#include "../../src/refresh_plan.h"

void setUp() {}
void tearDown() {}

// 默认节奏：窗口类 10 分钟，余额 50 秒；每行可以在行配置里覆盖。
void default_intervals_match_the_product_rules() {
  TEST_ASSERT_EQUAL_UINT32(600, quota::kWindowRefreshSeconds);
  TEST_ASSERT_EQUAL_UINT32(50, quota::kBalanceRefreshSeconds);
}

// dueAt 为 0 表示不在队列里；到点才到期，用无符号差值比较可跨 millis 回绕。
void due_handles_never_scheduled_and_wraparound() {
  TEST_ASSERT_FALSE(quota::refreshDue(0, 0));
  TEST_ASSERT_FALSE(quota::refreshDue(1000, 0));
  TEST_ASSERT_FALSE(quota::refreshDue(999, 1000));
  TEST_ASSERT_TRUE(quota::refreshDue(1000, 1000));
  TEST_ASSERT_TRUE(quota::refreshDue(1001, 1000));
  // 4000000000ms 到期，回绕后的 5000ms 已经过去。
  TEST_ASSERT_TRUE(quota::refreshDue(5000, 4000000000UL));
  TEST_ASSERT_FALSE(quota::refreshDue(4000000000UL - 1, 4000000000UL));
}

// 失败退避不短于该行配置的间隔，避免高频重试。
void retry_delay_never_goes_below_the_interval() {
  TEST_ASSERT_EQUAL_UINT32(600, quota::retryDelay(600, 30));
  TEST_ASSERT_EQUAL_UINT32(600, quota::retryDelay(600, 599));
  TEST_ASSERT_EQUAL_UINT32(600, quota::retryDelay(600, 600));
  TEST_ASSERT_EQUAL_UINT32(900, quota::retryDelay(600, 900));
  TEST_ASSERT_EQUAL_UINT32(50, quota::retryDelay(50, 30));
  TEST_ASSERT_EQUAL_UINT32(120, quota::retryDelay(50, 120));
  TEST_ASSERT_EQUAL_UINT32(20, quota::retryDelay(20, 5));
}

// 时钟未同步时不能用 Unix 冷却截止时间：否则一个过去的截止时间会被算成
// 几亿秒，把重试排到 7 天后（冷启动卡死的根因）。
void cooldown_requires_a_valid_clock() {
  const uint32_t blockedUntil = 1794407172;
  TEST_ASSERT_FALSE(quota::cooldownActive(false, blockedUntil, 0));
  TEST_ASSERT_EQUAL_UINT32(0, quota::cooldownRemainingSeconds(false, blockedUntil, 0));
  // 时钟有效时照常判断。
  TEST_ASSERT_TRUE(quota::cooldownActive(true, blockedUntil, blockedUntil - 1));
  TEST_ASSERT_EQUAL_UINT32(1, quota::cooldownRemainingSeconds(true, blockedUntil, blockedUntil - 1));
  TEST_ASSERT_FALSE(quota::cooldownActive(true, blockedUntil, blockedUntil));
  TEST_ASSERT_FALSE(quota::cooldownActive(true, blockedUntil, blockedUntil + 1));
  // 没有冷却（NVS 值为 0）时任何时钟都不算冷却。
  TEST_ASSERT_FALSE(quota::cooldownActive(true, 0, 1794407172));
  TEST_ASSERT_EQUAL_UINT32(0, quota::cooldownRemainingSeconds(true, 0, 1794407172));
}

// 退避不能把等待排到 7 天以上（scheduleSourceRetry 的 clamp 依赖它）。
void cooldown_remaining_is_bounded_by_the_deadline() {
  const uint32_t nowEpoch = 1794400000;
  TEST_ASSERT_EQUAL_UINT32(300, quota::cooldownRemainingSeconds(true, nowEpoch + 300, nowEpoch));
  TEST_ASSERT_TRUE(quota::cooldownRemainingSeconds(true, nowEpoch + 300, nowEpoch) < 7 * 86400UL);
}

// 多 key 里只坏一个时：这一行要显示异常，但仍按正常间隔继续抓。
// 如果按“失败”退避，几个回合后整组会被拖到 15 分钟，健康 key 的数据也跟着变旧。
void partial_key_failure_keeps_the_normal_cadence() {
  const quota::GroupDecision oneOfTwo = quota::groupDecision(2, 1);
  TEST_ASSERT_TRUE(oneOfTwo.showError);
  TEST_ASSERT_TRUE(oneOfTwo.keepsSchedule);
  const quota::GroupDecision oneOfFour = quota::groupDecision(4, 3);
  TEST_ASSERT_TRUE(oneOfFour.showError);
  TEST_ASSERT_TRUE(oneOfFour.keepsSchedule);
  // 部分成功走成功路径（sourcePartiallySucceeded 调 sourceSucceeded），
  // 调度不使用 retryDelay；这条只在固件里可验证，单测不重复断言恒真值。
}

// 全部 key 失败才允许退避；全成功则不显示异常。
void whole_group_failure_is_the_only_retry_case() {
  const quota::GroupDecision allFailed = quota::groupDecision(2, 2);
  TEST_ASSERT_TRUE(allFailed.showError);
  TEST_ASSERT_FALSE(allFailed.keepsSchedule);
  const quota::GroupDecision allOk = quota::groupDecision(2, 0);
  TEST_ASSERT_FALSE(allOk.showError);
  TEST_ASSERT_TRUE(allOk.keepsSchedule);
  TEST_ASSERT_FALSE(quota::groupDecision(0, 0).showError);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(default_intervals_match_the_product_rules);
  RUN_TEST(due_handles_never_scheduled_and_wraparound);
  RUN_TEST(retry_delay_never_goes_below_the_interval);
  RUN_TEST(cooldown_requires_a_valid_clock);
  RUN_TEST(cooldown_remaining_is_bounded_by_the_deadline);
  RUN_TEST(partial_key_failure_keeps_the_normal_cadence);
  RUN_TEST(whole_group_failure_is_the_only_retry_case);
  return UNITY_END();
}
