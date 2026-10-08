#pragma once

#include <cJSON.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <initializer_list>

namespace claude {
constexpr uint32_t kFiveHourSeconds = 5 * 3600;
constexpr uint32_t kSevenDaySeconds = 7 * 86400;
struct RateWindow {
  bool present = false;
  float usedPercent = 0;
  uint32_t resetAt = 0;
  uint32_t durationSeconds = 0;
};

inline bool integer(const cJSON* value, double min, double max) {
  return cJSON_IsNumber(value) && std::isfinite(value->valuedouble) &&
         value->valuedouble >= min && value->valuedouble <= max &&
         std::floor(value->valuedouble) == value->valuedouble;
}

// Gregorian calendar to Unix days; independent of the device's display timezone.
inline int64_t civilDays(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
  return era * 146097LL + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}

inline bool calendarEpoch(int y, int m, int d, int h, int min, int sec, int64_t& result) {
  static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (y < 1970 || y > 2106 || m < 1 || m > 12 || d < 1 || h < 0 || h > 23 ||
      min < 0 || min > 59 || sec < 0 || sec > 59) return false;
  const bool leap = y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
  if (d > days[m - 1] + (m == 2 && leap ? 1 : 0)) return false;
  result = civilDays(y, m, d) * 86400 + h * 3600 + min * 60 + sec;
  return true;
}

inline bool parseTimestamp(const char* text, uint32_t& result) {
  if (!text || std::strlen(text) < 20) return false;
  const int positions[] = {0,1,2,3,5,6,8,9,11,12,14,15,17,18};
  for (int i : positions) if (text[i] < '0' || text[i] > '9') return false;
  if (text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
      text[13] != ':' || text[16] != ':') return false;
  auto two = [&](int p) { return (text[p] - '0') * 10 + text[p + 1] - '0'; };
  int64_t seconds;
  if (!calendarEpoch(two(0) * 100 + two(2), two(5), two(8), two(11), two(14), two(17), seconds)) {
    return false;
  }
  const char* suffix = text + 19;
  if (*suffix == '.') {
    ++suffix;
    if (*suffix < '0' || *suffix > '9') return false;
    while (*suffix >= '0' && *suffix <= '9') ++suffix;
  }
  if (std::strcmp(suffix, "Z") != 0) {
    if (std::strlen(suffix) != 6 || (*suffix != '+' && *suffix != '-') || suffix[3] != ':') {
      return false;
    }
    for (int i : {1,2,4,5}) if (suffix[i] < '0' || suffix[i] > '9') return false;
    int hours = (suffix[1] - '0') * 10 + suffix[2] - '0';
    int minutes = (suffix[4] - '0') * 10 + suffix[5] - '0';
    if (hours > 23 || minutes > 59) return false;
    seconds -= (*suffix == '+' ? 1 : -1) * (hours * 3600 + minutes * 60);
  }
  if (seconds <= 0 || seconds > UINT32_MAX) return false;
  result = static_cast<uint32_t>(seconds);
  return true;
}

inline bool parseWindow(const cJSON* value, uint32_t duration, RateWindow& out) {
  if (!value || cJSON_IsNull(value)) { out = RateWindow{}; return true; }
  if (!cJSON_IsObject(value)) return false;
  const cJSON* used = cJSON_GetObjectItemCaseSensitive(value, "utilization");
  const cJSON* reset = cJSON_GetObjectItemCaseSensitive(value, "resets_at");
  if (!cJSON_IsNumber(used) || !std::isfinite(used->valuedouble) ||
      used->valuedouble < 0 || used->valuedouble > 100) return false;
  RateWindow candidate;
  candidate.present = true;
  candidate.usedPercent = static_cast<float>(used->valuedouble);
  candidate.durationSeconds = duration;
  if (reset && !cJSON_IsNull(reset) &&
      (!cJSON_IsString(reset) || !parseTimestamp(reset->valuestring, candidate.resetAt))) return false;
  out = candidate;
  return true;
}

inline bool parseUsage(const char* json, RateWindow& five, RateWindow& seven) {
  cJSON* root = cJSON_ParseWithOpts(json, nullptr, true);
  if (!root) return false;
  const cJSON* first = cJSON_GetObjectItemCaseSensitive(root, "five_hour");
  const cJSON* second = cJSON_GetObjectItemCaseSensitive(root, "seven_day");
  RateWindow newFive, newSeven;
  bool valid = cJSON_IsObject(root) && (first || second) &&
      parseWindow(first, kFiveHourSeconds, newFive) &&
      parseWindow(second, kSevenDaySeconds, newSeven);
  cJSON_Delete(root);
  if (!valid) return false;  // Never partially replace a last-good snapshot.
  five = newFive;
  seven = newSeven;
  return true;
}

// Retry-After accepts seconds or an IMF-fixdate. Ignore malformed values.
inline uint32_t retryAfter(const char* text, uint32_t now) {
  if (!text || !*text) return 0;
  uint64_t seconds = 0;
  bool numeric = true;
  for (const char* p = text; *p; ++p) {
    if (*p < '0' || *p > '9') { numeric = false; break; }
    seconds = seconds * 10 + (*p - '0');
    if (seconds > UINT32_MAX) return UINT32_MAX;
  }
  if (numeric) return static_cast<uint32_t>(seconds);
  char weekday[4] = {}, month[4] = {}, zone[4] = {};
  int d, y, h, m, s, count = 0;
  if (std::sscanf(text, "%3[A-Za-z], %d %3[A-Za-z] %d %d:%d:%d %3s%n", weekday, &d, month, &y, &h, &m, &s, zone, &count) != 8 ||
      text[count] || std::strcmp(zone, "GMT") != 0) return 0;
  const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
  int monthIndex = 0;
  for (int i = 0; i < 12; ++i) if (std::strcmp(month, months[i]) == 0) monthIndex = i + 1;
  int64_t epoch;
  if (!calendarEpoch(y, monthIndex, d, h, m, s, epoch) || epoch <= now || epoch > UINT32_MAX) return 0;
  return static_cast<uint32_t>(epoch - now);
}
}  // namespace claude
