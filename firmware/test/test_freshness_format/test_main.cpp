#include <unity.h>
#include "../../src/freshness_format.h"

void setUp() {}
void tearDown() {}

// 不足一个刷新间隔时精确分钟没有信息量：10 分钟显示 <10m；不到 1 分钟的间隔
// 用秒显示，避免 <1m / 0m 掩盖真实新鲜度。
void fresh_data_reports_the_interval_floor() {
  char output[12] = {};
  TEST_ASSERT_TRUE(quota::ageText(0, 600, output, sizeof(output)));
  TEST_ASSERT_EQUAL_STRING("<10m", output);
  TEST_ASSERT_TRUE(quota::ageText(599, 600, output, sizeof(output)));
  TEST_ASSERT_EQUAL_STRING("<10m", output);
  TEST_ASSERT_TRUE(quota::ageText(0, 50, output, sizeof(output)));
  TEST_ASSERT_EQUAL_STRING("<50s", output);
  TEST_ASSERT_TRUE(quota::ageText(49, 50, output, sizeof(output)));
  TEST_ASSERT_EQUAL_STRING("<50s", output);
  TEST_ASSERT_TRUE(quota::ageText(0, 60, output, sizeof(output)));
  TEST_ASSERT_EQUAL_STRING("<1m", output);
  TEST_ASSERT_TRUE(quota::ageText(0, 3600, output, sizeof(output)));
  TEST_ASSERT_EQUAL_STRING("<1h", output);
  TEST_ASSERT_TRUE(quota::ageText(0, 86400, output, sizeof(output)));
  TEST_ASSERT_EQUAL_STRING("<1d", output);
}

// 到达间隔后改为向下取整的分钟数，随时间增长，直接看出数据有多旧。
void stale_data_reports_whole_minutes() {
  char output[12] = {};
  const unsigned long elapsed[] = {600, 659, 660, 1234, 6000};
  const char* expected[] = {"10m", "10m", "11m", "20m", "100m"};
  for (size_t i = 0; i < sizeof(elapsed) / sizeof(elapsed[0]); ++i) {
    TEST_ASSERT_TRUE(quota::ageText(elapsed[i], 600, output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING(expected[i], output);
  }
  TEST_ASSERT_TRUE(quota::ageText(50, 50, output, sizeof(output)));
  TEST_ASSERT_EQUAL_STRING("50s", output);
  TEST_ASSERT_TRUE(quota::ageText(110, 50, output, sizeof(output)));
  TEST_ASSERT_EQUAL_STRING("110s", output);
  TEST_ASSERT_TRUE(quota::ageText(60, 60, output, sizeof(output)));
  TEST_ASSERT_EQUAL_STRING("1m", output);
  TEST_ASSERT_TRUE(quota::ageText(7200, 3600, output, sizeof(output)));
  TEST_ASSERT_EQUAL_STRING("2h", output);
  TEST_ASSERT_TRUE(quota::ageText(172800, 86400, output, sizeof(output)));
  TEST_ASSERT_EQUAL_STRING("2d", output);
}

void tiny_interval_renders_in_seconds() {
  char output[12] = {};
  TEST_ASSERT_TRUE(quota::ageText(5, 30, output, sizeof(output)));
  TEST_ASSERT_EQUAL_STRING("<30s", output);
  TEST_ASSERT_TRUE(quota::ageText(45, 30, output, sizeof(output)));
  TEST_ASSERT_EQUAL_STRING("45s", output);
  // 不能整除分钟的间隔退回秒，避免写出 <1m 这种不成立的阈值。
  TEST_ASSERT_TRUE(quota::ageText(80, 90, output, sizeof(output)));
  TEST_ASSERT_EQUAL_STRING("<90s", output);
  TEST_ASSERT_TRUE(quota::ageText(100, 90, output, sizeof(output)));
  TEST_ASSERT_EQUAL_STRING("100s", output);
  // 90 分钟能整除分钟，用 m。
  TEST_ASSERT_TRUE(quota::ageText(0, 5400, output, sizeof(output)));
  TEST_ASSERT_EQUAL_STRING("<90m", output);
}

// 还没有成功记录时显示 --，而不是拿 millis() 当起点算出一个假时长。
void missing_success_history_renders_a_placeholder() {
  char output[12] = {};
  TEST_ASSERT_TRUE(quota::ageSince(0, 123456, 600, output, sizeof(output)));
  TEST_ASSERT_EQUAL_STRING("--", output);
  TEST_ASSERT_TRUE(quota::ageSince(0, 0, 50, output, sizeof(output)));
  TEST_ASSERT_EQUAL_STRING("--", output);
}

// millis 回绕后仍按实际经过的时间计算。
void age_since_handles_millis_wraparound() {
  char output[12] = {};
  const uint32_t lastOk = 0xFFFFFF00u;  // 距离回绕 256ms
  TEST_ASSERT_TRUE(quota::ageSince(lastOk, 0x000003E8u, 600, output, sizeof(output)));
  TEST_ASSERT_EQUAL_STRING("<10m", output);
  TEST_ASSERT_TRUE(quota::ageSince(1000, 1000 + 600000, 600, output, sizeof(output)));
  TEST_ASSERT_EQUAL_STRING("10m", output);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(fresh_data_reports_the_interval_floor);
  RUN_TEST(stale_data_reports_whole_minutes);
  RUN_TEST(tiny_interval_renders_in_seconds);
  RUN_TEST(missing_success_history_renders_a_placeholder);
  RUN_TEST(age_since_handles_millis_wraparound);
  return UNITY_END();
}
