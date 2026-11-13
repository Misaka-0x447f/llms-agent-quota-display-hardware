#include <unity.h>
#include "../../src/row_config.h"

using quota::RowSource;
using quota::WindowKind;

void setUp() {}
void tearDown() {}

// 默认配置：ChatGPT 7 天、DeepSeek、OpenRouter，顺序即行序。
void default_rows_parse_in_order() {
  quota::RowConfig rows[quota::kMaxRows];
  size_t count = 0;
  TEST_ASSERT_TRUE(quota::parseRows(quota::kDefaultRows, strlen(quota::kDefaultRows), rows, count));
  TEST_ASSERT_EQUAL_UINT32(3, count);
  TEST_ASSERT_TRUE(rows[0].source == RowSource::kChatgpt);
  TEST_ASSERT_TRUE(rows[0].window == WindowKind::kSevenDay);
  TEST_ASSERT_TRUE(rows[1].source == RowSource::kDeepseek);
  TEST_ASSERT_TRUE(rows[2].source == RowSource::kOpenrouter);
}

void window_suffix_and_whitespace_are_accepted() {
  quota::RowConfig rows[quota::kMaxRows];
  size_t count = 0;
  const char* text = "  chatgpt:5h , claude:7d ";
  TEST_ASSERT_TRUE(quota::parseRows(text, strlen(text), rows, count));
  TEST_ASSERT_EQUAL_UINT32(2, count);
  TEST_ASSERT_TRUE(rows[0].source == RowSource::kChatgpt);
  TEST_ASSERT_TRUE(rows[0].window == WindowKind::kFiveHour);
  TEST_ASSERT_TRUE(rows[1].source == RowSource::kClaude);
  TEST_ASSERT_TRUE(rows[1].window == WindowKind::kSevenDay);

  // 同一来源可以占两行，窗口不同。
  const char* both = "chatgpt:5h,chatgpt:7d";
  TEST_ASSERT_TRUE(quota::parseRows(both, strlen(both), rows, count));
  TEST_ASSERT_EQUAL_UINT32(2, count);
  TEST_ASSERT_TRUE(rows[0].window == WindowKind::kFiveHour);
  TEST_ASSERT_TRUE(rows[1].window == WindowKind::kSevenDay);
}

void malformed_rows_are_rejected_whole() {
  quota::RowConfig rows[quota::kMaxRows];
  size_t count = 7;
  const char* invalid[] = {
      "", ",", "chatgpt,", ",chatgpt", "chatgpt,,deepseek", "chatgpt:",
      "chatgpt:1d", "chatgpt:7", "chatgpt:7d:5h", "deepseek:7d",
      "openrouter:5h", "CHATGPT", "chat gpt", "unknown",
      "chatgpt:7d,deepseek,openrouter,claude",
  };
  for (const char* text : invalid) {
    count = 7;
    TEST_ASSERT_FALSE(quota::parseRows(text, strlen(text), rows, count));
    TEST_ASSERT_EQUAL_UINT32(0, count);  // 全有或全无
  }
}

void rows_format_back_to_canonical_text() {
  quota::RowConfig rows[quota::kMaxRows];
  size_t count = 0;
  TEST_ASSERT_TRUE(quota::parseRows(quota::kDefaultRows, strlen(quota::kDefaultRows), rows, count));
  char out[64];
  TEST_ASSERT_TRUE(quota::formatRows(rows, count, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("chatgpt:7d@10m,deepseek@50s,openrouter@50s", out);

  const char* five = "claude:5h,chatgpt:5h";
  TEST_ASSERT_TRUE(quota::parseRows(five, strlen(five), rows, count));
  TEST_ASSERT_TRUE(quota::formatRows(rows, count, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("claude:5h@10m,chatgpt:5h@10m", out);

  // 缓冲区不够时返回 false，且不留半截配置。
  TEST_ASSERT_FALSE(quota::formatRows(rows, count, out, 8));
  TEST_ASSERT_EQUAL_STRING("", out);
}

// 行数不足 3 时整体居中，行距仍是 16px。
void layouts_center_short_row_sets() {
  quota::RowSlotLayout layout[quota::kMaxRows];
  TEST_ASSERT_TRUE(quota::rowLayout(3, layout));
  TEST_ASSERT_EQUAL_INT16(20, layout[0].textY);
  TEST_ASSERT_EQUAL_INT16(36, layout[1].textY);
  TEST_ASSERT_EQUAL_INT16(52, layout[2].textY);

  TEST_ASSERT_TRUE(quota::rowLayout(2, layout));
  TEST_ASSERT_EQUAL_INT16(28, layout[0].textY);
  TEST_ASSERT_EQUAL_INT16(44, layout[1].textY);

  TEST_ASSERT_TRUE(quota::rowLayout(1, layout));
  TEST_ASSERT_EQUAL_INT16(36, layout[0].textY);

  TEST_ASSERT_FALSE(quota::rowLayout(0, layout));
  TEST_ASSERT_FALSE(quota::rowLayout(4, layout));
}

// 线在本行墨迹之下、不压下一行、不出屏、每条线独占一个 8px 页。
void layouts_keep_lines_clear_and_on_separate_pages() {
  for (size_t count = 1; count <= quota::kMaxRows; ++count) {
    quota::RowSlotLayout layout[quota::kMaxRows];
    TEST_ASSERT_TRUE(quota::rowLayout(count, layout));
    TEST_ASSERT_TRUE(quota::rowLayoutFits(count, layout));
    for (size_t i = 0; i < count; ++i) {
      TEST_ASSERT_EQUAL_INT16(layout[i].textY + 2, layout[i].lineY);
      TEST_ASSERT_TRUE(layout[i].lineY > layout[i].textY + 1);
      TEST_ASSERT_TRUE(layout[i].lineY < 64);
    }
    for (size_t i = 1; i < count; ++i) {
      TEST_ASSERT_EQUAL_INT16(16, layout[i].textY - layout[i - 1].textY);
    }
  }
}

// 布局校验本身要能抓住错误，而不是恒真。
void layout_check_rejects_unsafe_rows() {
  quota::RowSlotLayout layout[3] = {{20, 22}, {36, 38}, {52, 54}};
  TEST_ASSERT_TRUE(quota::rowLayoutFits(3, layout));

  quota::RowSlotLayout overlapping[3] = {{20, 21}, {36, 38}, {52, 54}};
  TEST_ASSERT_FALSE(quota::rowLayoutFits(3, overlapping));  // 线压在本行字上

  quota::RowSlotLayout crossing[2] = {{20, 26}, {36, 38}};
  TEST_ASSERT_FALSE(quota::rowLayoutFits(2, crossing));  // 线越过下一行墨迹

  quota::RowSlotLayout off_screen[1] = {{56, 64}};
  TEST_ASSERT_FALSE(quota::rowLayoutFits(1, off_screen));  // 线出屏
}

// 每行可以覆盖抓取间隔：`@30s` / `@5m` / `@1h`；省略时按来源取默认值。
void per_row_intervals_parse_and_default() {
  quota::RowConfig rows[quota::kMaxRows];
  size_t count = 0;
  const char* text = "chatgpt:7d@5m,deepseek@30s";
  TEST_ASSERT_TRUE(quota::parseRows(text, strlen(text), rows, count));
  TEST_ASSERT_EQUAL_UINT32(300, rows[0].intervalSeconds);
  TEST_ASSERT_EQUAL_UINT32(30, rows[1].intervalSeconds);

  const char* defaults = "chatgpt:7d,deepseek";
  TEST_ASSERT_TRUE(quota::parseRows(defaults, strlen(defaults), rows, count));
  TEST_ASSERT_EQUAL_UINT32(quota::kWindowRefreshSeconds, rows[0].intervalSeconds);
  TEST_ASSERT_EQUAL_UINT32(quota::kBalanceRefreshSeconds, rows[1].intervalSeconds);

  const char* hour = "claude@1h";
  TEST_ASSERT_TRUE(quota::parseRows(hour, strlen(hour), rows, count));
  TEST_ASSERT_EQUAL_UINT32(3600, rows[0].intervalSeconds);

  // 允许范围的边界值：10 秒与 24 小时。
  const char* edges = "chatgpt:7d@10s,deepseek@24h";
  TEST_ASSERT_TRUE(quota::parseRows(edges, strlen(edges), rows, count));
  TEST_ASSERT_EQUAL_UINT32(10, rows[0].intervalSeconds);
  TEST_ASSERT_EQUAL_UINT32(86400, rows[1].intervalSeconds);
}

void malformed_intervals_are_rejected() {
  quota::RowConfig rows[quota::kMaxRows];
  size_t count = 7;
  const char* invalid[] = {
      "chatgpt:7d@", "chatgpt:7d@0s", "chatgpt:7d@5", "chatgpt:7d@5x",
      "chatgpt:7d@5m30s", "chatgpt:7d@-5m", "chatgpt:7d@5m,", "chatgpt:7d@9s",
      "chatgpt:7d@25h", "chatgpt@5m@30s",
  };
  for (const char* text : invalid) {
    count = 7;
    TEST_ASSERT_FALSE(quota::parseRows(text, strlen(text), rows, count));
    TEST_ASSERT_EQUAL_UINT32(0, count);
  }
}

// 同一来源占两行时抓取节奏取最短，两行都不会比用户设定的更旧。
void shared_source_uses_the_shortest_interval() {
  quota::RowConfig rows[quota::kMaxRows];
  size_t count = 0;
  const char* text = "chatgpt:5h@1h,chatgpt:7d@5m,deepseek";
  TEST_ASSERT_TRUE(quota::parseRows(text, strlen(text), rows, count));
  TEST_ASSERT_EQUAL_UINT32(300, quota::sourceIntervalSeconds(rows, count, quota::RowSource::kChatgpt));
  TEST_ASSERT_EQUAL_UINT32(50, quota::sourceIntervalSeconds(rows, count, quota::RowSource::kDeepseek));
  TEST_ASSERT_EQUAL_UINT32(600, quota::sourceIntervalSeconds(rows, count, quota::RowSource::kClaude));
}

// 回显总是带上实际生效的间隔，读的人不用再记默认值。
void rows_format_always_includes_the_interval() {
  quota::RowConfig rows[quota::kMaxRows];
  size_t count = 0;
  char out[96];
  const char* text = "chatgpt:7d,deepseek@30s";
  TEST_ASSERT_TRUE(quota::parseRows(text, strlen(text), rows, count));
  TEST_ASSERT_TRUE(quota::formatRows(rows, count, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("chatgpt:7d@10m,deepseek@30s", out);

  const char* defaults = "chatgpt:7d,deepseek,openrouter";
  TEST_ASSERT_TRUE(quota::parseRows(defaults, strlen(defaults), rows, count));
  TEST_ASSERT_TRUE(quota::formatRows(rows, count, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("chatgpt:7d@10m,deepseek@50s,openrouter@50s", out);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(default_rows_parse_in_order);
  RUN_TEST(window_suffix_and_whitespace_are_accepted);
  RUN_TEST(malformed_rows_are_rejected_whole);
  RUN_TEST(rows_format_back_to_canonical_text);
  RUN_TEST(layouts_center_short_row_sets);
  RUN_TEST(layouts_keep_lines_clear_and_on_separate_pages);
  RUN_TEST(layout_check_rejects_unsafe_rows);
  RUN_TEST(per_row_intervals_parse_and_default);
  RUN_TEST(malformed_intervals_are_rejected);
  RUN_TEST(shared_source_uses_the_shortest_interval);
  RUN_TEST(rows_format_always_includes_the_interval);
  return UNITY_END();
}

