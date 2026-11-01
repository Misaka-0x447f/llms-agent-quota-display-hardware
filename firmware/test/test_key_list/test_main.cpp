#include <unity.h>
#include "../../src/key_list.h"

void setUp() {}
void tearDown() {}

void splits_on_newlines_and_commas() {
  key_list::List list;
  TEST_ASSERT_TRUE(key_list::parse("sk-one\nsk-two", 13, list));
  TEST_ASSERT_EQUAL_UINT32(2, list.count);
  TEST_ASSERT_EQUAL_STRING("sk-one", list.items[0]);
  TEST_ASSERT_EQUAL_STRING("sk-two", list.items[1]);

  const char* mixed = "sk-one,sk-two\r\nsk-three";
  TEST_ASSERT_TRUE(key_list::parse(mixed, std::strlen(mixed), list));
  TEST_ASSERT_EQUAL_UINT32(3, list.count);
  TEST_ASSERT_EQUAL_STRING("sk-three", list.items[2]);
}

void trims_whitespace_and_drops_empty_entries() {
  key_list::List list;
  const char* text = "  sk-one \t, ,\n\n  sk-two  \n,";
  TEST_ASSERT_TRUE(key_list::parse(text, std::strlen(text), list));
  TEST_ASSERT_EQUAL_UINT32(2, list.count);
  TEST_ASSERT_EQUAL_STRING("sk-one", list.items[0]);
  TEST_ASSERT_EQUAL_STRING("sk-two", list.items[1]);
}

void keeps_only_the_first_occurrence_of_a_key() {
  key_list::List list;
  const char* text = "sk-one,sk-two,sk-one";
  TEST_ASSERT_TRUE(key_list::parse(text, std::strlen(text), list));
  TEST_ASSERT_EQUAL_UINT32(2, list.count);
  TEST_ASSERT_EQUAL_STRING("sk-one", list.items[0]);
  TEST_ASSERT_EQUAL_STRING("sk-two", list.items[1]);
}

void accepts_empty_lists_and_four_keys() {
  key_list::List list;
  TEST_ASSERT_TRUE(key_list::parse("", 0, list));
  TEST_ASSERT_EQUAL_UINT32(0, list.count);
  TEST_ASSERT_TRUE(key_list::parse(nullptr, 0, list));
  TEST_ASSERT_EQUAL_UINT32(0, list.count);

  const char* four = "a,b,c,d";
  TEST_ASSERT_TRUE(key_list::parse(four, std::strlen(four), list));
  TEST_ASSERT_EQUAL_UINT32(4, list.count);
  TEST_ASSERT_EQUAL_STRING("d", list.items[3]);
}

void rejects_more_keys_or_longer_keys_than_supported() {
  key_list::List list;
  const char* five = "a,b,c,d,e";
  TEST_ASSERT_FALSE(key_list::parse(five, std::strlen(five), list));

  char longKey[key_list::kMaxKeyLength + 2];
  std::memset(longKey, 'k', sizeof(longKey));
  longKey[sizeof(longKey) - 1] = '\0';
  TEST_ASSERT_FALSE(key_list::parse(longKey, key_list::kMaxKeyLength + 1, list));

  char limitKey[key_list::kMaxKeyLength + 1];
  std::memset(limitKey, 'k', key_list::kMaxKeyLength);
  limitKey[key_list::kMaxKeyLength] = '\0';
  TEST_ASSERT_TRUE(key_list::parse(limitKey, key_list::kMaxKeyLength, list));
  TEST_ASSERT_EQUAL_UINT32(1, list.count);
}

void duplicated_keys_do_not_consume_the_limit() {
  key_list::List list;
  const char* text = "a,a,a,b,c,d";
  TEST_ASSERT_TRUE(key_list::parse(text, std::strlen(text), list));
  TEST_ASSERT_EQUAL_UINT32(4, list.count);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(splits_on_newlines_and_commas);
  RUN_TEST(trims_whitespace_and_drops_empty_entries);
  RUN_TEST(keeps_only_the_first_occurrence_of_a_key);
  RUN_TEST(accepts_empty_lists_and_four_keys);
  RUN_TEST(rejects_more_keys_or_longer_keys_than_supported);
  RUN_TEST(duplicated_keys_do_not_consume_the_limit);
  return UNITY_END();
}
