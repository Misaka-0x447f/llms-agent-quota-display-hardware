#pragma once

#include <cstddef>
#include <cstring>

// 一个平台可以配置多个 API key。key 之间用换行或逗号分隔，两端空白忽略，
// 空项丢弃，重复 key 只保留第一次出现的位置。
namespace key_list {
constexpr size_t kMaxKeys = 4;
constexpr size_t kMaxKeyLength = 128;

struct List {
  char items[kMaxKeys][kMaxKeyLength + 1];
  size_t count = 0;
};

inline bool duplicate(const List& list, const char* text, size_t length) {
  for (size_t i = 0; i < list.count; ++i) {
    if (std::strlen(list.items[i]) == length &&
        std::memcmp(list.items[i], text, length) == 0) {
      return true;
    }
  }
  return false;
}

// 只做本地切分，不校验 key 的上游格式。超过 kMaxKeys 个不同 key 或单个 key
// 超过 kMaxKeyLength 字节时返回 false，调用方应拒绝整份配置。
inline bool parse(const char* text, size_t length, List& out) {
  out.count = 0;
  if (text == nullptr) return true;
  size_t i = 0;
  while (i < length) {
    size_t begin = i;
    while (i < length && text[i] != '\n' && text[i] != '\r' && text[i] != ',') ++i;
    size_t end = i;
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
    if (end > begin) {
      const size_t keyLength = end - begin;
      if (keyLength > kMaxKeyLength) return false;
      if (!duplicate(out, text + begin, keyLength)) {
        if (out.count >= kMaxKeys) return false;
        std::memcpy(out.items[out.count], text + begin, keyLength);
        out.items[out.count][keyLength] = '\0';
        ++out.count;
      }
    }
    if (i < length) ++i;
  }
  return true;
}
}  // namespace key_list
