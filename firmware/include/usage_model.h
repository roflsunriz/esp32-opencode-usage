#pragma once

#include <cmath>
#include <cstdint>

namespace usage_model {

constexpr uint8_t kProtocolVersion = 1;
constexpr uint8_t kPeriodCount = 3;

struct PeriodUsage {
  double used = 0.0;
  double limit = 0.0;
  double percent = 0.0;
  int64_t resetInSec = -1;
  bool valid = false;
};

struct UsageSnapshot {
  uint64_t updatedAt = 0;
  PeriodUsage rolling;
  PeriodUsage weekly;
  PeriodUsage monthly;
  bool valid = false;
};

inline bool isFinite(double value) { return std::isfinite(value); }

// Values are normalized by the host. A usage amount can exceed its limit;
// that state is shown as 100% filled while retaining the original dollar
// amount and percentage in the text and ACK.
inline bool setPeriod(PeriodUsage &period, double used, double limit,
                      double percent, bool hasPercent, int64_t resetInSec,
                      bool hasResetInSec) {
  if (!isFinite(used) || !isFinite(limit) || used < 0.0 || limit <= 0.0) {
    return false;
  }

  const double derivedPercent = (used / limit) * 100.0;
  const double selectedPercent = hasPercent ? percent : derivedPercent;
  if (!isFinite(selectedPercent) || selectedPercent < 0.0 ||
      selectedPercent > 100000.0) {
    return false;
  }
  if (hasResetInSec && resetInSec < 0) {
    return false;
  }

  period.used = used;
  period.limit = limit;
  period.percent = selectedPercent;
  period.resetInSec = hasResetInSec ? resetInSec : -1;
  period.valid = true;
  return true;
}

inline uint16_t filledWidth(double percent, uint16_t width) {
  if (!isFinite(percent) || percent <= 0.0 || width == 0) {
    return 0;
  }
  if (percent >= 100.0) {
    return width;
  }
  return static_cast<uint16_t>((percent * static_cast<double>(width) / 100.0) +
                               0.5);
}

inline bool isStale(uint32_t elapsedMs, uint32_t pollIntervalSec) {
  const uint32_t thresholdSec =
      pollIntervalSec > 90 ? pollIntervalSec * 2 : 180;
  return elapsedMs >= thresholdSec * 1000UL;
}

inline const char *periodLabel(uint8_t index) {
  switch (index) {
  case 0:
    return "5H";
  case 1:
    return "WEEK";
  case 2:
    return "MONTH";
  default:
    return "?";
  }
}

} // namespace usage_model
