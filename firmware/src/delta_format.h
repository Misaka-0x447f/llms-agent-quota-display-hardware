#pragma once

#include <math.h>
#include <stddef.h>
#include <stdio.h>

namespace quota {
// Only deltas within one hour use five-minute steps. Round ties away from zero;
// normalize negative zero, and retain minutes even when rounding reaches 60m.
inline bool formatMinuteDelta(float seconds, char* output, size_t size) {
  const float magnitude = fabsf(seconds);
  if (!(magnitude <= 3600.0f)) return false;
  const unsigned minutes = static_cast<unsigned>(floorf(magnitude / 300.0f + 0.5f)) * 5;
  const char sign = seconds < 0 && minutes != 0 ? '-' : '+';
  snprintf(output, size, "%c%um", sign, minutes);
  return true;
}
}  // namespace quota
