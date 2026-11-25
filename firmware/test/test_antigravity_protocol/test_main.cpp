#include <unity.h>
#include "../../src/antigravity_protocol.h"

void setUp() {}
void tearDown() {}

// 真实形状（protobuf JSON，小驼峰）：groups[].buckets[]，remainingFraction 是剩余比例。
const char* kSummary =
    R"({"groups":[{"displayName":"Gemini","buckets":[)"
    R"({"bucketId":"gemini-5h","window":"5h","remainingFraction":0.8,"resetTime":"2026-11-25T21:00:00Z"},)"
    R"({"bucketId":"gemini-weekly","window":"weekly","remainingFraction":0.37,"resetTime":"2026-11-30T08:00:00Z"})"
    R"(]}]})";

// 每周桶映射成 7d 窗口：used = (1 - remaining) * 100，resetTime 换成 Unix 秒。
void weekly_bucket_becomes_seven_day_window() {
  antigravity::RateWindow seven;
  TEST_ASSERT_TRUE(antigravity::parseQuotaSummary(kSummary, seven));
  TEST_ASSERT_TRUE(seven.present);
  TEST_ASSERT_EQUAL_FLOAT(63.0f, seven.usedPercent);
  TEST_ASSERT_EQUAL_UINT32(7 * 86400, seven.durationSeconds);
  TEST_ASSERT_EQUAL_UINT32(1796025600U, seven.resetAt);  // 2026-11-30T08:00:00Z
}

// 只认 Gemini 组：Claude/GPT 的 3p-weekly 再低也不能顶替。
void third_party_buckets_are_ignored() {
  const char* json =
      R"({"groups":[{"buckets":[{"bucketId":"3p-weekly","window":"weekly",)"
      R"("remainingFraction":0.01,"resetTime":"2026-11-30T08:00:00Z"}]}]})";
  antigravity::RateWindow seven;
  TEST_ASSERT_FALSE(antigravity::parseQuotaSummary(json, seven));
}

// 分模型/分档位的 weekly 桶不能被当成 Gemini 组额度。
void model_specific_weekly_buckets_are_not_the_group_quota() {
  const char* json =
      R"({"groups":[{"buckets":[{"bucketId":"gemini-4-argon-weekly","window":"weekly",)"
      R"("remainingFraction":0.9,"resetTime":"2026-11-30T08:00:00Z"}]}]})";
  antigravity::RateWindow seven;
  TEST_ASSERT_FALSE(antigravity::parseQuotaSummary(json, seven));
}

// 上游可能外包一层 {"response": {...}}，也可能回 snake_case。
void wrapped_and_snake_case_payloads_are_accepted() {
  const char* json =
      R"({"response":{"groups":[{"buckets":[{"bucket_id":"gemini-weekly",)"
      R"("remaining_fraction":0.5,"reset_time":"2026-11-30T08:00:00Z"}]}]}})";
  antigravity::RateWindow seven;
  TEST_ASSERT_TRUE(antigravity::parseQuotaSummary(json, seven));
  TEST_ASSERT_EQUAL_FLOAT(50.0f, seven.usedPercent);
}

// 只有 resetTime 没有 remainingFraction 的桶是「已耗尽」，不是未知。
void exhausted_bucket_without_fraction_counts_as_full_usage() {
  const char* json =
      R"({"groups":[{"buckets":[{"bucketId":"gemini-weekly",)"
      R"("resetTime":"2026-11-30T08:00:00Z"}]}]})";
  antigravity::RateWindow seven;
  TEST_ASSERT_TRUE(antigravity::parseQuotaSummary(json, seven));
  TEST_ASSERT_EQUAL_FLOAT(100.0f, seven.usedPercent);
}

// 没有任何一个桶带重置时间时不能编造窗口。
void bucket_without_fraction_or_reset_is_not_a_window() {
  const char* json = R"({"groups":[{"buckets":[{"bucketId":"gemini-weekly"}]}]})";
  antigravity::RateWindow seven;
  TEST_ASSERT_FALSE(antigravity::parseQuotaSummary(json, seven));
}

// 非法响应（坏 JSON、越界比例、缺组）一律失败，调用方保留旧值。
void malformed_summaries_are_rejected() {
  antigravity::RateWindow seven;
  TEST_ASSERT_FALSE(antigravity::parseQuotaSummary("{not json", seven));
  TEST_ASSERT_FALSE(antigravity::parseQuotaSummary(R"({"groups":[]})", seven));
  TEST_ASSERT_FALSE(antigravity::parseQuotaSummary(
      R"({"groups":[{"buckets":[{"bucketId":"gemini-weekly","remainingFraction":1.4,)"
      R"("resetTime":"2026-11-30T08:00:00Z"}]}]})",
      seven));
}

// 档位：paidTier 优先于 currentTier（免费账号也会报 currentTier=free-tier）。
void plan_prefers_paid_tier() {
  char plan[48] = {};
  TEST_ASSERT_TRUE(antigravity::parsePlan(
      R"({"currentTier":{"name":"Free"},"paidTier":{"name":"Google AI Pro"}})", plan,
      sizeof(plan)));
  TEST_ASSERT_EQUAL_STRING("Google AI Pro", plan);
  TEST_ASSERT_TRUE(antigravity::parsePlan(R"({"currentTier":{"name":"free-tier"}})", plan,
                                         sizeof(plan)));
  TEST_ASSERT_EQUAL_STRING("free-tier", plan);
  TEST_ASSERT_FALSE(antigravity::parsePlan(R"({"currentTier":{}})", plan, sizeof(plan)));
}

// 充值余额在 paidTier.availableCredits 里；creditAmount 是 int64，JSON 里是字符串。
void credits_are_converted_from_hundredths() {
  const char* json =
      R"({"paidTier":{"name":"Google AI Pro","availableCredits":[)"
      R"({"creditType":"GOOGLE_ONE_AI","creditAmount":"1234","minimumCreditAmountForUsage":"500"})"
      R"(]}})";
  antigravity::Credits credits;
  TEST_ASSERT_TRUE(antigravity::parseCredits(json, credits) ==
                   antigravity::CreditsStatus::kFound);
  TEST_ASSERT_TRUE(credits.present);
  TEST_ASSERT_EQUAL_FLOAT(12.34, credits.amount);
  TEST_ASSERT_EQUAL_FLOAT(5.0, credits.minimum);
}

// 只认 GOOGLE_ONE_AI；其它类型、缺字段、数字形式都要处理正确。
// 解析失败时调用方保留旧值，所以失败的用例用独立对象检查 present。
void credits_accept_numbers_and_ignore_other_types() {
  antigravity::Credits credits;
  TEST_ASSERT_TRUE(antigravity::parseCredits(
      R"({"paidTier":{"availableCredits":[{"creditType":"GOOGLE_ONE_AI","credit_amount":250,)"
      R"("minimum_credit_amount_for_usage":0}]}})",
      credits) == antigravity::CreditsStatus::kFound);
  TEST_ASSERT_EQUAL_FLOAT(2.5, credits.amount);
  TEST_ASSERT_EQUAL_FLOAT(0.0, credits.minimum);
  // 合法响应里只有别的币种：明确「没有这一项」，调用方要清空旧值。
  antigravity::Credits other;
  TEST_ASSERT_TRUE(antigravity::parseCredits(
      R"({"paidTier":{"availableCredits":[{"creditType":"OTHER","creditAmount":"99"}]}})",
      other) == antigravity::CreditsStatus::kAbsent);
  TEST_ASSERT_FALSE(other.present);
  antigravity::Credits empty;
  TEST_ASSERT_TRUE(antigravity::parseCredits(R"({"paidTier":{}})", empty) ==
                   antigravity::CreditsStatus::kAbsent);
  TEST_ASSERT_FALSE(empty.present);
  // 响应本身无效（空/截断/金额坏掉）：调用方要保留旧值，不能当成「没有余额」。
  antigravity::Credits invalid;
  TEST_ASSERT_TRUE(antigravity::parseCredits("", invalid) ==
                   antigravity::CreditsStatus::kInvalid);
  TEST_ASSERT_TRUE(antigravity::parseCredits("{\"paidTier\":", invalid) ==
                   antigravity::CreditsStatus::kInvalid);
  TEST_ASSERT_TRUE(antigravity::parseCredits(
      R"({"paidTier":{"availableCredits":[{"creditType":"GOOGLE_ONE_AI","creditAmount":"oops"}]}})",
      invalid) == antigravity::CreditsStatus::kInvalid);
  TEST_ASSERT_FALSE(invalid.present);
  // 字段在但类型错（paidTier 是数组、列表里混进 null）也是坏响应，
  // 不能当成「明确没有余额」而清掉上一次的真实金额。
  antigravity::Credits wrongTier;
  TEST_ASSERT_TRUE(antigravity::parseCredits(R"({"paidTier":[]})", wrongTier) ==
                   antigravity::CreditsStatus::kInvalid);
  TEST_ASSERT_TRUE(antigravity::parseCredits(
      R"({"paidTier":{"availableCredits":[null]}})", wrongTier) ==
                   antigravity::CreditsStatus::kInvalid);
  // 坏包装同样不能算「没有余额」。
  TEST_ASSERT_TRUE(antigravity::parseCredits(R"({"response":[]})", wrongTier) ==
                   antigravity::CreditsStatus::kInvalid);
  TEST_ASSERT_FALSE(wrongTier.present);
}

// 真实响应里「余额 0」只有 minimum：按 0 处理；两个字段都缺或显式 null 不算。
void zero_balance_is_recognised_only_with_a_minimum() {
  antigravity::Credits credits;
  TEST_ASSERT_TRUE(antigravity::parseCredits(
      R"({"paidTier":{"availableCredits":[{"creditType":"GOOGLE_ONE_AI",)"
      R"("minimumCreditAmountForUsage":"50"}]}})",
      credits) == antigravity::CreditsStatus::kFound);
  TEST_ASSERT_TRUE(credits.present);
  TEST_ASSERT_EQUAL_FLOAT(0.0, credits.amount);
  TEST_ASSERT_EQUAL_FLOAT(0.5, credits.minimum);

  // 只有 creditType：形状不认识，保留旧值。
  antigravity::Credits bare;
  TEST_ASSERT_TRUE(antigravity::parseCredits(
      R"({"paidTier":{"availableCredits":[{"creditType":"GOOGLE_ONE_AI"}]}})", bare) ==
                   antigravity::CreditsStatus::kInvalid);

  // 显式 null 的 amount、没有 minimum：同样是坏数据。
  antigravity::Credits nullAmount;
  TEST_ASSERT_TRUE(antigravity::parseCredits(
      R"({"paidTier":{"availableCredits":[{"creditType":"GOOGLE_ONE_AI","creditAmount":null}]}})",
      nullAmount) == antigravity::CreditsStatus::kInvalid);

  // 坏响应不能覆盖已有的非零余额（解析失败时调用方保留旧值）。
  antigravity::Credits previous;
  previous.present = true;
  previous.amount = 12.34;
  TEST_ASSERT_TRUE(antigravity::parseCredits(
      R"({"paidTier":{"availableCredits":[{"creditType":"GOOGLE_ONE_AI"}]}})", previous) ==
                   antigravity::CreditsStatus::kInvalid);
  TEST_ASSERT_TRUE(previous.present);
  TEST_ASSERT_EQUAL_FLOAT(12.34, previous.amount);
}

// 切换条件：只有周额度用尽才切到充值余额（余额低于 minimum 不再触发切换）。
void overage_switches_only_on_exhaustion() {
  antigravity::RateWindow seven;
  antigravity::Credits credits;
  TEST_ASSERT_TRUE(antigravity::parseQuotaSummary(kSummary, seven));
  // 还有 37% 额度，也没有充值余额信息：正常显示百分比。
  TEST_ASSERT_FALSE(antigravity::overageMode(seven, credits));

  // 余额低于门槛但额度还有 37%：**不切换**（用户 2026-11-25 撤回该规则）。
  credits.present = true;
  credits.amount = 0.0;
  credits.minimum = 5.0;
  TEST_ASSERT_FALSE(antigravity::overageMode(seven, credits));

  // 额度用尽：切到金额，不管余额多少。
  seven.usedPercent = 100.0f;
  TEST_ASSERT_TRUE(antigravity::overageMode(seven, credits));
  credits.amount = 50.0;
  TEST_ASSERT_TRUE(antigravity::overageMode(seven, credits));

  // 没有窗口数据时不做切换（这一行走占位符）。
  seven.present = false;
  TEST_ASSERT_FALSE(antigravity::overageMode(seven, credits));

  // 余额和门槛都不参与判断：只要额度还有剩余就不切。
  seven.present = true;
  seven.usedPercent = 10.0f;
  credits.minimum = 0;
  credits.amount = 0.0;
  TEST_ASSERT_FALSE(antigravity::overageMode(seven, credits));
}

// 切换阈值与屏幕取整一致：剩余 0.5% 仍显示 1%，不能已经切成余额。
void overage_threshold_matches_the_rounded_percent() {
  antigravity::RateWindow seven;
  seven.present = true;
  seven.usedPercent = 99.0f;  // 剩余 1%
  antigravity::Credits credits;
  TEST_ASSERT_FALSE(antigravity::overageMode(seven, credits));
  seven.usedPercent = 99.5f;  // 剩余 0.5%，屏幕上四舍五入是 1%，仍然不切
  TEST_ASSERT_FALSE(antigravity::overageMode(seven, credits));
  seven.usedPercent = 99.6f;  // 剩余 0.4%，显示 0%，切换
  TEST_ASSERT_TRUE(antigravity::overageMode(seven, credits));
  seven.usedPercent = 100.0f;
  TEST_ASSERT_TRUE(antigravity::overageMode(seven, credits));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(weekly_bucket_becomes_seven_day_window);
  RUN_TEST(third_party_buckets_are_ignored);
  RUN_TEST(model_specific_weekly_buckets_are_not_the_group_quota);
  RUN_TEST(wrapped_and_snake_case_payloads_are_accepted);
  RUN_TEST(exhausted_bucket_without_fraction_counts_as_full_usage);
  RUN_TEST(bucket_without_fraction_or_reset_is_not_a_window);
  RUN_TEST(malformed_summaries_are_rejected);
  RUN_TEST(plan_prefers_paid_tier);
  RUN_TEST(credits_are_converted_from_hundredths);
  RUN_TEST(credits_accept_numbers_and_ignore_other_types);
  RUN_TEST(zero_balance_is_recognised_only_with_a_minimum);
  RUN_TEST(overage_switches_only_on_exhaustion);
  RUN_TEST(overage_threshold_matches_the_rounded_percent);
  return UNITY_END();
}
