#include <unity.h>
#include "../../src/balance_protocol.h"

void setUp() {}
void tearDown() {}

void deepseek_reads_only_cny_regardless_of_order_or_usd_value() {
  balance_protocol::Balance out;
  TEST_ASSERT_FALSE(out.present);
  const char* examples[] = {
    R"({"balance_infos":[{"currency":"CNY","total_balance":"110.00"},{"currency":"USD","total_balance":"12.34"}]})",
    R"({"balance_infos":[{"currency":"USD","total_balance":0},{"currency":"CNY","total_balance":"110.00"}]})",
    R"({"balance_infos":[{"currency":"USD","total_balance":"bad"},{"currency":"CNY","total_balance":"110.00"}]})"
  };
  for (const char* json : examples) {
    TEST_ASSERT_TRUE(balance_protocol::parseDeepSeek(json, out));
    TEST_ASSERT_TRUE(out.present);
    TEST_ASSERT_EQUAL_FLOAT(110, out.remaining);
    TEST_ASSERT_EQUAL_STRING("CNY", out.currency);
  }
  TEST_ASSERT_TRUE(balance_protocol::parseDeepSeek(R"({"balance_infos":[{"currency":"CNY","total_balance":0},{"currency":"USD","total_balance":50}]})", out));
  TEST_ASSERT_EQUAL_FLOAT(0, out.remaining);
  TEST_ASSERT_EQUAL_STRING("CNY", out.currency);
  TEST_ASSERT_TRUE(balance_protocol::parseDeepSeek(R"({"is_available":false,"balance_infos":[{"currency":"CNY","total_balance":"-0.10"}]})", out));
  TEST_ASSERT_EQUAL_FLOAT(-0.10, out.remaining);
}

void deepseek_rejects_bad_payloads_without_replacing_balance() {
  balance_protocol::Balance out;
  TEST_ASSERT_TRUE(balance_protocol::parseDeepSeek(R"({"balance_infos":[{"currency":"CNY","total_balance":"19.25"}]})", out));
  const char* invalid[] = {nullptr, "", "{}", "[]", "not-json",
    R"({"balance_infos":[{"currency":"USD","total_balance":"99.00"}]})",
    R"({"balance_infos":[]})", R"({"balance_infos":{}})",
    R"({"balance_infos":[{"currency":"EUR","total_balance":"20"}]})",
    R"({"balance_infos":[{"currency":"CNY","total_balance":null}]})",
    R"({"balance_infos":[{"currency":"CNY","total_balance":true}]})",
    R"({"balance_infos":[{"currency":"CNY","total_balance":""}]})",
    R"({"balance_infos":[{"currency":"CNY","total_balance":"NaN"}]})",
    R"({"balance_infos":[{"currency":"CNY","total_balance":"inf"}]})",
    R"({"balance_infos":[{"currency":"CNY","total_balance":"1e999"}]})",
    R"({"balance_infos":[{"currency":"CNY","total_balance":1e999}]})",
    R"({"balance_infos":[{"currency":"CNY","total_balance":"0x10"}]})",
    R"({"balance_infos":[{"currency":"CNY","total_balance":" 10"}]})",
    R"({"balance_infos":[{"currency":"CNY","total_balance":"10junk"}]})",
    R"({"balance_infos":[{"currency":"CNY","total_balance":"1."}]})",
    R"({"balance_infos":[{"currency":"CNY","total_balance":"1e"}]})",
    R"({"balance_infos":[{"currency":"CNY","total_balance":"bad"},{"currency":"USD","total_balance":"10"}]})",
    R"({"balance_infos":[{"currency":"CNY","total_balance":"10"}]}trailing)"};
  for (const char* json : invalid) {
    TEST_ASSERT_FALSE(balance_protocol::parseDeepSeek(json, out));
    TEST_ASSERT_TRUE(out.present);
    TEST_ASSERT_EQUAL_FLOAT(19.25, out.remaining);
    TEST_ASSERT_EQUAL_STRING("CNY", out.currency);
  }
}

void openrouter_subtracts_usage_and_preserves_zero_and_debt() {
  balance_protocol::Balance out;
  TEST_ASSERT_TRUE(balance_protocol::parseOpenRouter(R"({"data":{"total_credits":100.5,"total_usage":25.75}})", out));
  TEST_ASSERT_TRUE(out.present);
  TEST_ASSERT_EQUAL_FLOAT(74.75, out.remaining);
  TEST_ASSERT_EQUAL_STRING("USD", out.currency);
  TEST_ASSERT_TRUE(balance_protocol::parseOpenRouter(R"({"data":{"total_credits":10,"total_usage":10}})", out));
  TEST_ASSERT_EQUAL_FLOAT(0, out.remaining);
  TEST_ASSERT_TRUE(balance_protocol::parseOpenRouter(R"({"data":{"total_credits":10,"total_usage":10.25}})", out));
  TEST_ASSERT_EQUAL_FLOAT(-0.25, out.remaining);
}

void openrouter_rejects_bad_payloads_without_replacing_balance() {
  balance_protocol::Balance out;
  TEST_ASSERT_TRUE(balance_protocol::parseOpenRouter(R"({"data":{"total_credits":10,"total_usage":1}})", out));
  const char* invalid[] = {nullptr, "", "{}", "[]", "not-json",
    R"({"error":{"code":403,"message":"Management key required"}})",
    R"({"data":{}})", R"({"data":[]})",
    R"({"data":{"total_credits":10}})",
    R"({"data":{"total_credits":"10","total_usage":1}})",
    R"({"data":{"total_credits":10,"total_usage":null}})",
    R"({"data":{"total_credits":10,"total_usage":true}})",
    R"({"data":{"total_credits":1e999,"total_usage":1}})",
    R"({"data":{"total_credits":1,"total_usage":1e999}})",
    R"({"data":{"total_credits":1e308,"total_usage":-1e308}})",
    R"({"data":{"total_credits":10,"total_usage":1}}trailing)"};
  for (const char* json : invalid) {
    TEST_ASSERT_FALSE(balance_protocol::parseOpenRouter(json, out));
    TEST_ASSERT_TRUE(out.present);
    TEST_ASSERT_EQUAL_FLOAT(9, out.remaining);
    TEST_ASSERT_EQUAL_STRING("USD", out.currency);
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(deepseek_reads_only_cny_regardless_of_order_or_usd_value);
  RUN_TEST(deepseek_rejects_bad_payloads_without_replacing_balance);
  RUN_TEST(openrouter_subtracts_usage_and_preserves_zero_and_debt);
  RUN_TEST(openrouter_rejects_bad_payloads_without_replacing_balance);
  return UNITY_END();
}
