#include <unity.h>
#include "../../src/codex_protocol.h"

void setUp() {}
void tearDown() {}

const char* kBoth =
    R"({"rate_limit":{"primary_window":{"used_percent":23.5,"reset_at":1794550000,"limit_window_seconds":18000},"secondary_window":{"used_percent":80,"reset_at":1795000000,"limit_window_seconds":604800}}})";

void windows_are_mapped_by_duration() {
  codex::RateWindow five, seven;
  TEST_ASSERT_TRUE(codex::parseUsage(kBoth, five, seven));
  TEST_ASSERT_TRUE(five.present);
  TEST_ASSERT_EQUAL_FLOAT(23.5, five.usedPercent);
  TEST_ASSERT_EQUAL_UINT32(5 * 3600, five.durationSeconds);
  TEST_ASSERT_EQUAL_UINT32(1794550000, five.resetAt);
  TEST_ASSERT_TRUE(seven.present);
  TEST_ASSERT_EQUAL_FLOAT(80, seven.usedPercent);
  TEST_ASSERT_EQUAL_UINT32(7 * 86400, seven.durationSeconds);
  TEST_ASSERT_EQUAL_UINT32(1795000000, seven.resetAt);
}

// 只返回 7 天窗口的账号也要能用；缺的窗口不保留旧值（本轮没有就是没有）。
void only_seven_day_is_accepted() {
  codex::RateWindow five, seven;
  TEST_ASSERT_TRUE(codex::parseUsage(
      R"({"rate_limit":{"secondary_window":{"used_percent":12,"limit_window_seconds":604800}}})",
      five, seven));
  TEST_ASSERT_FALSE(five.present);
  TEST_ASSERT_TRUE(seven.present);
  TEST_ASSERT_EQUAL_FLOAT(12, seven.usedPercent);
  TEST_ASSERT_EQUAL_UINT32(0, seven.resetAt);  // reset_at 可省略
}

// 未知窗口长度忽略，但已知窗口照常采用；null 窗口视为没有。
void unknown_windows_are_ignored() {
  codex::RateWindow five, seven;
  TEST_ASSERT_TRUE(codex::parseUsage(
      R"({"rate_limit":{"primary_window":{"used_percent":5,"limit_window_seconds":2592000},"secondary_window":{"used_percent":40,"limit_window_seconds":604800}}})",
      five, seven));
  TEST_ASSERT_FALSE(five.present);
  TEST_ASSERT_TRUE(seven.present);
  TEST_ASSERT_TRUE(codex::parseUsage(
      R"({"rate_limit":{"primary_window":null,"secondary_window":{"used_percent":40,"limit_window_seconds":604800}}})",
      five, seven));
  TEST_ASSERT_FALSE(five.present);
  TEST_ASSERT_TRUE(seven.present);
}

// 解析失败时不能部分覆盖上一次成功的数据。
void malformed_responses_preserve_last_good() {
  codex::RateWindow five, seven;
  TEST_ASSERT_TRUE(codex::parseUsage(kBoth, five, seven));
  const char* invalid[] = {
      "", "not-json", "[]", "{}", R"({"rate_limit":null})",
      R"({"rate_limit":{}})", R"({"rate_limit":{"primary_window":{}}})",
      R"({"rate_limit":{"primary_window":{"used_percent":-1,"limit_window_seconds":18000}}})",
      R"({"rate_limit":{"primary_window":{"used_percent":"20","limit_window_seconds":18000}}})",
      R"({"rate_limit":{"primary_window":{"used_percent":20,"limit_window_seconds":"18000"}}})",
      R"({"rate_limit":{"primary_window":{"used_percent":20,"limit_window_seconds":0}}})",
      R"({"rate_limit":{"secondary_window":{"used_percent":20,"reset_at":"soon","limit_window_seconds":604800}}})",
      R"json({"rate_limit":{"secondary_window":{"used_percent":20,"limit_window_seconds":604800}}})trailing)json",
  };
  for (const char* json : invalid) {
    TEST_ASSERT_FALSE(codex::parseUsage(json, five, seven));
    TEST_ASSERT_EQUAL_FLOAT(23.5, five.usedPercent);
    TEST_ASSERT_EQUAL_FLOAT(80, seven.usedPercent);
  }
}

// 超额使用返回 >100 时按 100 截断，而不是丢掉整份响应（剩余量按 0 处理）。
void overage_is_clamped_to_full_usage() {
  codex::RateWindow five, seven;
  TEST_ASSERT_TRUE(codex::parseUsage(
      R"({"rate_limit":{"secondary_window":{"used_percent":137.5,"limit_window_seconds":604800}}})",
      five, seven));
  TEST_ASSERT_TRUE(seven.present);
  TEST_ASSERT_EQUAL_FLOAT(100, seven.usedPercent);
}

// 5h / 7d 的先后顺序不影响映射，未知长度两个都忽略时整份响应无效。
void mapping_does_not_depend_on_window_order() {
  codex::RateWindow five, seven;
  TEST_ASSERT_TRUE(codex::parseUsage(
      R"({"rate_limit":{"primary_window":{"used_percent":70,"limit_window_seconds":604800},"secondary_window":{"used_percent":20,"limit_window_seconds":18000}}})",
      five, seven));
  TEST_ASSERT_TRUE(five.present);
  TEST_ASSERT_TRUE(seven.present);
  TEST_ASSERT_EQUAL_FLOAT(20, five.usedPercent);
  TEST_ASSERT_EQUAL_FLOAT(70, seven.usedPercent);
  TEST_ASSERT_FALSE(codex::parseUsage(
      R"({"rate_limit":{"primary_window":{"used_percent":1,"limit_window_seconds":2592000},"secondary_window":{"used_percent":2,"limit_window_seconds":2592001}}})",
      five, seven));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(windows_are_mapped_by_duration);
  RUN_TEST(only_seven_day_is_accepted);
  RUN_TEST(unknown_windows_are_ignored);
  RUN_TEST(malformed_responses_preserve_last_good);
  RUN_TEST(overage_is_clamped_to_full_usage);
  RUN_TEST(mapping_does_not_depend_on_window_order);
  return UNITY_END();
}
