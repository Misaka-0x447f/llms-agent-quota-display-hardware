#include <unity.h>
#include "../../src/delta_format.h"

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

int main() {
  UNITY_BEGIN();
  RUN_TEST(delta_rounds_to_five_minutes_with_symmetric_ties);
  RUN_TEST(longer_deltas_keep_the_existing_duration_formatter);
  return UNITY_END();
}
