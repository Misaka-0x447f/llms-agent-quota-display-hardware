#include <unity.h>
#include "../../src/balance_format.h"

void setUp() {}
void tearDown() {}

void single_key_keeps_two_decimals() {
  balance_protocol::Balance balances[1];
  balances[0].remaining = 12.34;
  balances[0].present = true;
  char out[64];
  TEST_ASSERT_TRUE(balance_format::joined(balances, 1, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("12.34", out);
}

void two_keys_keep_one_decimal_and_use_plus() {
  balance_protocol::Balance balances[2];
  balances[0].remaining = 12.0;
  balances[0].present = true;
  balances[1].remaining = 24.0;
  balances[1].present = true;
  char out[64];
  TEST_ASSERT_TRUE(balance_format::joined(balances, 2, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("12.0+24.0", out);
}

void multiple_keys_round_each_amount_independently() {
  balance_protocol::Balance balances[3];
  balances[0].remaining = 12.34;
  balances[0].present = true;
  balances[1].remaining = 24.56;
  balances[1].present = true;
  balances[2].remaining = -0.24;
  balances[2].present = true;
  char out[64];
  TEST_ASSERT_TRUE(balance_format::joined(balances, 3, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("12.3+24.6+-0.2", out);
}

void unknown_balances_render_as_placeholders() {
  balance_protocol::Balance balances[2];
  balances[0].remaining = 12.0;
  balances[0].present = true;
  balances[1].present = false;
  char out[64];
  TEST_ASSERT_TRUE(balance_format::joined(balances, 2, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("12.0+--", out);

  TEST_ASSERT_TRUE(balance_format::joined(balances, 0, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("--", out);
}

void rejects_output_buffers_that_are_too_small() {
  balance_protocol::Balance balances[2];
  balances[0].remaining = 12.0;
  balances[0].present = true;
  balances[1].remaining = 24.0;
  balances[1].present = true;
  char out[8];
  TEST_ASSERT_FALSE(balance_format::joined(balances, 2, out, sizeof(out)));
}

void four_keys_fit_the_display_buffer() {
  balance_protocol::Balance balances[4];
  for (size_t i = 0; i < 4; ++i) {
    balances[i].remaining = 12.0 + static_cast<double>(i) * 12.0;
    balances[i].present = true;
  }
  char out[96];
  TEST_ASSERT_TRUE(balance_format::joined(balances, 4, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("12.0+24.0+36.0+48.0", out);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(single_key_keeps_two_decimals);
  RUN_TEST(two_keys_keep_one_decimal_and_use_plus);
  RUN_TEST(multiple_keys_round_each_amount_independently);
  RUN_TEST(unknown_balances_render_as_placeholders);
  RUN_TEST(rejects_output_buffers_that_are_too_small);
  RUN_TEST(four_keys_fit_the_display_buffer);
  return UNITY_END();
}
