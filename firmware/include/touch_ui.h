#pragma once

#include <cstddef>
#include <cstdint>

#include "backlight_timer.h"

namespace touch_ui {

constexpr uint16_t kDisplayWidth = 320;
constexpr uint16_t kDisplayHeight = 240;
constexpr uint16_t kTabHeight = 40;
constexpr uint16_t kFooterTop = 228;
constexpr uint16_t kTabWidth = kDisplayWidth / 2;

// ESP32-2432S028R with ILI9341 rotation(1). The raw range is the published
// ESPHome XPT2046 calibration for the 25/33/32/39 touch bus. On the actual
// 2026-09-09 board, Display-tab taps measured rawX≈500..800 and
// rawY≈2500..2900, proving that the screen axes are swapped with no mirror.
// These values apply only to this board wiring and rotation. Touch diagnostics
// expose raw and mapped values so a board revision can be recalibrated from
// evidence rather than guesswork.
constexpr uint16_t kRawXMin = 280;
constexpr uint16_t kRawXMax = 3860;
constexpr uint16_t kRawYMin = 340;
constexpr uint16_t kRawYMax = 3860;

constexpr uint16_t kGridLeft = 5;
constexpr uint16_t kGridTop = 45;
constexpr uint16_t kGridColumns = 3;
constexpr uint16_t kGridRows = 3;
constexpr uint16_t kButtonWidth = 100;
constexpr uint16_t kButtonHeight = 50;
constexpr uint16_t kGridGapX = 5;
constexpr uint16_t kGridGapY = 6;
constexpr uint32_t kReleaseStableMs = 20;

enum class Tab : uint8_t { kUsage, kDisplay };
enum class ActionKind : uint8_t { kNone, kUsageTab, kDisplayTab, kTimeout };

struct Action {
  Action(ActionKind selectedKind = ActionKind::kNone,
         uint32_t selectedTimeoutSec = 0)
      : kind(selectedKind), timeoutSec(selectedTimeoutSec) {}

  ActionKind kind;
  uint32_t timeoutSec;
};

struct Point {
  uint16_t rawX = 0;
  uint16_t rawY = 0;
  uint16_t x = 0;
  uint16_t y = 0;
};

// A conversion can briefly change PENIRQ. Accept one action per physical
// contact and reopen only after the line has remained HIGH long enough.
class ReleaseLatch {
public:
  bool accept() {
    if (latched_) {
      return false;
    }
    latched_ = true;
    highSeen_ = false;
    return true;
  }

  void update(bool penLow, uint32_t nowMs) {
    if (!latched_) {
      return;
    }
    if (penLow) {
      highSeen_ = false;
      return;
    }
    if (!highSeen_) {
      highSeen_ = true;
      highSinceMs_ = nowMs;
      return;
    }
    if (static_cast<uint32_t>(nowMs - highSinceMs_) >= kReleaseStableMs) {
      latched_ = false;
      highSeen_ = false;
    }
  }

  bool isLatched() const { return latched_; }

private:
  bool latched_ = false;
  bool highSeen_ = false;
  uint32_t highSinceMs_ = 0;
};

inline bool isPlausibleRaw(uint16_t rawX, uint16_t rawY) {
  return rawX >= 50 && rawX <= 4090 && rawY >= 50 && rawY <= 4090;
}

inline uint16_t clamp(uint16_t value, uint16_t minimum, uint16_t maximum) {
  return value < minimum ? minimum : (value > maximum ? maximum : value);
}

inline uint16_t scaleRounded(uint16_t value, uint16_t minimum, uint16_t maximum,
                             uint16_t outputMaximum) {
  const uint32_t numerator =
      static_cast<uint32_t>(value - minimum) * outputMaximum;
  const uint32_t denominator = maximum - minimum;
  return static_cast<uint16_t>((numerator + denominator / 2) / denominator);
}

inline Point mapPoint(uint16_t rawX, uint16_t rawY, bool flipped = false) {
  const uint16_t boundedX = clamp(rawX, kRawXMin, kRawXMax);
  const uint16_t boundedY = clamp(rawY, kRawYMin, kRawYMax);
  Point point;
  point.rawX = rawX;
  point.rawY = rawY;
  point.x = scaleRounded(boundedY, kRawYMin, kRawYMax, kDisplayWidth - 1);
  point.y = scaleRounded(boundedX, kRawXMin, kRawXMax, kDisplayHeight - 1);
  if (flipped) {
    point.x = kDisplayWidth - 1 - point.x;
    point.y = kDisplayHeight - 1 - point.y;
  }
  return point;
}

inline uint16_t mapAxisCalibrated(uint16_t raw, int16_t start, int16_t end,
                                   int16_t targetStart, int16_t targetEnd,
                                   uint16_t maximum) {
  const int32_t span = static_cast<int32_t>(end) - start;
  if (span > -100 && span < 100) return targetStart;
  int32_t value = targetStart + (static_cast<int32_t>(raw) - start) *
                                  (targetEnd - targetStart) / span;
  if (value < 0) value = 0;
  if (value > maximum) value = maximum;
  return static_cast<uint16_t>(value);
}

inline Point mapPointCalibrated(uint16_t rawX, uint16_t rawY, bool flipped,
                                int16_t left, int16_t right,
                                int16_t top, int16_t bottom) {
  Point point;
  point.rawX = rawX;
  point.rawY = rawY;
  // 実機で確認した軸入れ替えを保つ。保存値は通常向きの物理座標。
  point.x = mapAxisCalibrated(rawY, left, right, 24, 295, 319);
  point.y = mapAxisCalibrated(rawX, top, bottom, 24, 215, 239);
  if (flipped) {
    point.x = 319 - point.x;
    point.y = 239 - point.y;
  }
  return point;
}

inline int16_t pressureThresholdFor(int16_t weakestPressure) {
  int16_t threshold = weakestPressure / 2;
  if (threshold < 12) threshold = 12;
  if (threshold > 120) threshold = 120;
  return threshold;
}

inline Action hitTest(Tab activeTab, uint16_t x, uint16_t y) {
  if (y < kTabHeight) {
    return Action(
        x < kTabWidth ? ActionKind::kUsageTab : ActionKind::kDisplayTab, 0);
  }
  if (activeTab != Tab::kDisplay || y < kGridTop || y >= kFooterTop) {
    return Action();
  }
  for (uint16_t row = 0; row < kGridRows; ++row) {
    const uint16_t top = kGridTop + row * (kButtonHeight + kGridGapY);
    if (y < top || y >= top + kButtonHeight) {
      continue;
    }
    for (uint16_t column = 0; column < kGridColumns; ++column) {
      const uint16_t left = kGridLeft + column * (kButtonWidth + kGridGapX);
      if (x >= left && x < left + kButtonWidth) {
        const size_t index = row * kGridColumns + column;
        return Action(ActionKind::kTimeout,
                      backlight_timer::kTimeoutOptionsSec[index]);
      }
    }
  }
  return Action();
}

inline const char *timeoutLabel(uint32_t timeoutSec) {
  switch (timeoutSec) {
  case 15:
    return "15s";
  case 30:
    return "30s";
  case 60:
    return "1m";
  case 120:
    return "2m";
  case 300:
    return "5m";
  case 600:
    return "10m";
  case 1800:
    return "30m";
  case 3600:
    return "1h";
  case 7200:
    return "2h";
  default:
    return "?";
  }
}

} // namespace touch_ui
