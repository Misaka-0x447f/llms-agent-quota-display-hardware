#include <unity.h>
#include "../../src/claude_protocol.h"

void setUp() {}
void tearDown() {}

void timestamps_are_utc_and_strict() {
  uint32_t epoch = 0;
  TEST_ASSERT_TRUE(claude::parseTimestamp("2026-10-08T00:00:00Z", epoch));
  TEST_ASSERT_EQUAL_UINT32(1791417600, epoch);
  uint32_t offset = 0;
  TEST_ASSERT_TRUE(claude::parseTimestamp("2026-10-08T08:00:00.123456+08:00", offset));
  TEST_ASSERT_EQUAL_UINT32(epoch, offset);
  TEST_ASSERT_TRUE(claude::parseTimestamp("2026-10-07T19:00:00-05:00", offset));
  TEST_ASSERT_EQUAL_UINT32(epoch, offset);
  TEST_ASSERT_TRUE(claude::parseTimestamp("2024-02-29T00:00:00Z", offset));
  const char* invalid[] = {"", "2026-02-29T00:00:00Z", "2026-10-08T24:00:00Z",
    "2026-10-08T00:00:00", "2026-10-08T00:00:00.Z", "2026-10-08T00:00:00Ztrailing",
    "2026-10-08T00:00:00+24:00", "2106-12-31T00:00:00Z"};
  for (auto text : invalid) TEST_ASSERT_FALSE(claude::parseTimestamp(text, offset));
}

void snapshots_accept_optional_windows_and_preserve_last_good() {
  claude::RateWindow five, seven;
  TEST_ASSERT_TRUE(claude::parseUsage(R"({"five_hour":{"utilization":23.5,"resets_at":"2026-10-08T00:00:00Z"},"seven_day":{"utilization":80,"resets_at":null}})", five, seven));
  TEST_ASSERT_EQUAL_FLOAT(23.5, five.usedPercent);
  TEST_ASSERT_EQUAL_UINT32(5 * 3600, five.durationSeconds);
  TEST_ASSERT_EQUAL_UINT32(7 * 86400, seven.durationSeconds);
  TEST_ASSERT_EQUAL_UINT32(0, seven.resetAt);
  const char* invalid[] = {"{}", "[]", "not-json",
    R"({"five_hour":{"utilization":1},"seven_day":{"utilization":101}})",
    R"({"five_hour":{"utilization":-1}})", R"({"five_hour":{"utilization":"20"}})",
    R"({"five_hour":{"utilization":12,"resets_at":123}})",
    R"({"five_hour":{"utilization":12,"resets_at":"bad-date"}})",
    R"({"five_hour":{}})", R"({"five_hour":null}trailing)"};
  for (auto json : invalid) {
    TEST_ASSERT_FALSE(claude::parseUsage(json, five, seven));
    TEST_ASSERT_EQUAL_FLOAT(23.5, five.usedPercent);
    TEST_ASSERT_EQUAL_FLOAT(80, seven.usedPercent);
  }
  TEST_ASSERT_TRUE(claude::parseUsage(R"({"seven_day":{"utilization":0}})", five, seven));
  TEST_ASSERT_FALSE(five.present);
  TEST_ASSERT_TRUE(seven.present);
  TEST_ASSERT_EQUAL_FLOAT(0, seven.usedPercent);
  TEST_ASSERT_TRUE(claude::parseUsage(R"({"five_hour":null,"seven_day":null})", five, seven));
  TEST_ASSERT_FALSE(five.present);
  TEST_ASSERT_FALSE(seven.present);
}

void retry_after_handles_seconds_and_http_dates() {
  const uint32_t now = 1791417600;
  TEST_ASSERT_EQUAL_UINT32(120, claude::retryAfter("120", now));
  TEST_ASSERT_EQUAL_UINT32(300, claude::retryAfter("Thu, 08 Oct 2026 00:05:00 GMT", now));
  TEST_ASSERT_EQUAL_UINT32(0, claude::retryAfter("Thu, 08 Oct 2026 00:00:00 GMT", now));
  TEST_ASSERT_EQUAL_UINT32(0, claude::retryAfter("-1", now));
  TEST_ASSERT_EQUAL_UINT32(0, claude::retryAfter("120junk", now));
  TEST_ASSERT_EQUAL_UINT32(0, claude::retryAfter("Thu, 08 Oct 2026 00:05:00 GMTjunk", now));
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, claude::retryAfter("999999999999999999", now));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(timestamps_are_utc_and_strict);
  RUN_TEST(snapshots_accept_optional_windows_and_preserve_last_good);
  RUN_TEST(retry_after_handles_seconds_and_http_dates);
  return UNITY_END();
}
