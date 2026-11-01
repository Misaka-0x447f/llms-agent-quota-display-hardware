#include <unity.h>
#include "../../src/delta_format.h"
#include "../../src/quota_settings.h"

void setUp() {}
void tearDown() {}

void delta_rounds_to_five_minutes_with_symmetric_ties() {
  struct Example { float seconds; const char* expected; };
  const Example examples[] = {
    {0, "+0m"}, {-0.0f, "+0m"}, {149.9f, "+0m"}, {-149.9f, "+0m"},
    {150, "+5m"}, {-150, "-5m"}, {180, "+5m"}, {-180, "-5m"},
    {449.9f, "+5m"}, {-449.9f, "-5m"}, {450, "+10m"}, {-450, "-10m"},
    {3449, "+55m"}, {-3449, "-55m"}, {3450, "+60m"}, {-3450, "-60m"},
    {3600, "+60m"}, {-3600, "-60m"}
  };
  for (const auto& example : examples) {
    char output[8] = {};
    TEST_ASSERT_TRUE(quota::formatMinuteDelta(example.seconds, output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING(example.expected, output);
  }
}

void longer_deltas_keep_the_existing_duration_formatter() {
  char output[8] = "same";
  TEST_ASSERT_FALSE(quota::formatMinuteDelta(3600.1f, output, sizeof(output)));
  TEST_ASSERT_FALSE(quota::formatMinuteDelta(-3600.1f, output, sizeof(output)));
  TEST_ASSERT_FALSE(quota::formatMinuteDelta(86400, output, sizeof(output)));
  TEST_ASSERT_EQUAL_STRING("same", output);
}

void interval_defaults_to_five_minutes_and_supports_legacy_minutes() {
  TEST_ASSERT_EQUAL_UINT32(300, quota::kDefaultRefreshSeconds);
  struct Example { const char* json; uint32_t seconds; };
  const Example examples[] = {
    {"{}", 300}, {R"({"refresh_seconds":50})", 50},
    {R"({"refresh_seconds":300})", 300},
    {R"({"refresh_seconds":86400})", 86400},
    {R"({"refresh_minutes":1})", 60}, {R"({"refresh_minutes":1440})", 86400}
  };
  for (const auto& example : examples) {
    uint32_t seconds = quota::kDefaultRefreshSeconds;
    cJSON* root = cJSON_Parse(example.json);
    TEST_ASSERT_TRUE(quota::readRefreshSeconds(root, seconds));
    TEST_ASSERT_EQUAL_UINT32(example.seconds, seconds);
    cJSON_Delete(root);
  }
  TEST_ASSERT_TRUE(quota::validRefreshSeconds(50));
  TEST_ASSERT_TRUE(quota::validRefreshSeconds(86400));
  TEST_ASSERT_FALSE(quota::validRefreshSeconds(49));
  TEST_ASSERT_FALSE(quota::validRefreshSeconds(86401));
}

void invalid_or_ambiguous_intervals_preserve_the_previous_setting() {
  const char* examples[] = {
    R"({"refresh_seconds":49})", R"({"refresh_seconds":86401})",
    R"({"refresh_seconds":50.5})", R"({"refresh_seconds":-50})",
    R"({"refresh_seconds":"50"})", R"({"refresh_seconds":null})",
    R"({"refresh_seconds":true})", R"({"refresh_seconds":1e100})",
    R"({"refresh_minutes":0})", R"({"refresh_minutes":1441})",
    R"({"refresh_minutes":1.5})", R"({"refresh_seconds":50,"refresh_minutes":1})"
  };
  for (const auto json : examples) {
    uint32_t seconds = 300;
    cJSON* root = cJSON_Parse(json);
    TEST_ASSERT_FALSE(quota::readRefreshSeconds(root, seconds));
    TEST_ASSERT_EQUAL_UINT32(300, seconds);
    cJSON_Delete(root);
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(delta_rounds_to_five_minutes_with_symmetric_ties);
  RUN_TEST(longer_deltas_keep_the_existing_duration_formatter);
  RUN_TEST(interval_defaults_to_five_minutes_and_supports_legacy_minutes);
  RUN_TEST(invalid_or_ambiguous_intervals_preserve_the_previous_setting);
  return UNITY_END();
}
