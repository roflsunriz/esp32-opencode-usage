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

// Calibration cross targets in rotation(1) coordinates. The first two share a
// row and the first and third share a column, so each screen axis is measured
// from its own pair. Two diagonal points cannot tell a swapped panel from a
// straight one (both read small/small then large/large), which is why a third
// point is required.
constexpr int16_t kCalibTargetX0 = 24;
constexpr int16_t kCalibTargetX1 = 295;
constexpr int16_t kCalibTargetY0 = 24;
constexpr int16_t kCalibTargetY1 = 215;
constexpr int16_t kCalibMinSpan = 1000;

enum class Tab : uint8_t { kUsage, kDisplay };
enum class ActionKind : uint8_t {
  kNone,
  kUsageTab,
  kDisplayTab,
  kTimeout,
  kSleepMinutes,
  kSleepHours,
  kPollInterval,
  kDisplayScroll
};

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

// A measured raw pair from one calibration cross.
struct RawXY {
  RawXY() = default;
  RawXY(int16_t xValue, int16_t yValue) : x(xValue), y(yValue) {}

  int16_t x = 0;
  int16_t y = 0;
};

// Per-board axis mapping. useRawXForX selects which raw axis drives screen x
// (and symmetrically for y), so swapped, straight, and mirrored panels share
// one code path. Bounds are raw values at the calibration targets and always
// refer to the normal (non-flipped) orientation.
struct CalibratedMap {
  bool useRawXForX = false;
  int16_t xStart = 0;
  int16_t xEnd = 0;
  bool useRawYForY = false;
  int16_t yStart = 0;
  int16_t yEnd = 0;
};

// Derives the axis mapping from three crosses: p1 at (X0, Y0), p2 at
// (X1, Y0), p3 at (X0, Y1). Each screen axis takes the raw axis that varies
// most across its own pair.
inline CalibratedMap buildCalibratedMap(RawXY p1, RawXY p2, RawXY p3) {
  CalibratedMap map;
  const int32_t rowSpanX =
      static_cast<int32_t>(p2.x) - static_cast<int32_t>(p1.x);
  const int32_t rowSpanY =
      static_cast<int32_t>(p2.y) - static_cast<int32_t>(p1.y);
  if ((rowSpanX < 0 ? -rowSpanX : rowSpanX) >=
      (rowSpanY < 0 ? -rowSpanY : rowSpanY)) {
    map.useRawXForX = true;
    map.xStart = p1.x;
    map.xEnd = p2.x;
  } else {
    map.useRawXForX = false;
    map.xStart = p1.y;
    map.xEnd = p2.y;
  }
  const int32_t columnSpanX =
      static_cast<int32_t>(p3.x) - static_cast<int32_t>(p1.x);
  const int32_t columnSpanY =
      static_cast<int32_t>(p3.y) - static_cast<int32_t>(p1.y);
  if ((columnSpanY < 0 ? -columnSpanY : columnSpanY) >=
      (columnSpanX < 0 ? -columnSpanX : columnSpanX)) {
    map.useRawYForY = true;
    map.yStart = p1.y;
    map.yEnd = p3.y;
  } else {
    map.useRawYForY = false;
    map.yStart = p1.x;
    map.yEnd = p3.x;
  }
  return map;
}

inline int32_t calibratedSpan(int16_t start, int16_t end) {
  return static_cast<int32_t>(end) - static_cast<int32_t>(start);
}

inline bool calibratedSpansValid(const CalibratedMap &map) {
  const int32_t xSpan = calibratedSpan(map.xStart, map.xEnd);
  const int32_t ySpan = calibratedSpan(map.yStart, map.yEnd);
  const int32_t xAbs = xSpan < 0 ? -xSpan : xSpan;
  const int32_t yAbs = ySpan < 0 ? -ySpan : ySpan;
  return xAbs > kCalibMinSpan && yAbs > kCalibMinSpan;
}

inline Point mapPointWithMap(uint16_t rawX, uint16_t rawY, bool flipped,
                             const CalibratedMap &map) {
  Point point;
  point.rawX = rawX;
  point.rawY = rawY;
  const uint16_t xRaw = map.useRawXForX ? rawX : rawY;
  const uint16_t yRaw = map.useRawYForY ? rawY : rawX;
  point.x = mapAxisCalibrated(xRaw, map.xStart, map.xEnd, kCalibTargetX0,
                              kCalibTargetX1, kDisplayWidth - 1);
  point.y = mapAxisCalibrated(yRaw, map.yStart, map.yEnd, kCalibTargetY0,
                              kCalibTargetY1, kDisplayHeight - 1);
  if (flipped) {
    point.x = kDisplayWidth - 1 - point.x;
    point.y = kDisplayHeight - 1 - point.y;
  }
  return point;
}

// Converts a version-1 record (fixed axis swap) to the general map so stored
// calibrations keep working after the update.
inline CalibratedMap calibratedFromLegacy(int16_t left, int16_t right,
                                          int16_t top, int16_t bottom) {
  CalibratedMap map;
  map.useRawXForX = false;
  map.xStart = left;
  map.xEnd = right;
  map.useRawYForY = false;
  map.yStart = top;
  map.yEnd = bottom;
  return map;
}

inline int16_t pressureThresholdFor(int16_t weakestPressure) {
  int16_t threshold = weakestPressure / 2;
  if (threshold < 12) threshold = 12;
  if (threshold > 120) threshold = 120;
  return threshold;
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

// New slider-based Display-tab settings (esp32-ui-style.md). The tab content
// is taller than the visible area, so the controller keeps a scroll offset.
// Slider tracks share one horizontal geometry; each row is identified by its
// content-space center Y.
constexpr int16_t kSliderTrackX0 = 14;
constexpr int16_t kSliderTrackX1 = 296;
constexpr int16_t kSliderThumbHalfW = 8;
constexpr int16_t kSliderTouchHalfH = 14;
constexpr int16_t kSliderMinutesY = 78;
constexpr int16_t kSliderHoursY = 128;
constexpr int16_t kSliderPollY = 178;
constexpr int16_t kDisplayContentHeight = 240;
constexpr int16_t kDisplayVisibleHeight = kFooterTop - kTabHeight; // 188
constexpr int16_t kDisplayScrollMax =
    kDisplayContentHeight - kDisplayVisibleHeight; // 52
// Scrollbar on the right edge of the Display tab. Tap above/below the thumb
// to page the content; the sliders handle their own taps.
constexpr int16_t kScrollBarX0 = 306;
constexpr int16_t kScrollBarY0 = 44;
constexpr int16_t kScrollBarY1 = 224;
constexpr int16_t kScrollPage = 40;

inline int16_t clampScroll(int32_t scroll) {
  if (scroll < 0) return 0;
  if (scroll > kDisplayScrollMax) return kDisplayScrollMax;
  return static_cast<int16_t>(scroll);
}

// Maps a screen X tap to a slider value, quantized to step. minV == maxV is
// treated as a fixed slider.
inline uint32_t sliderValueFromX(int16_t x, uint32_t minV, uint32_t maxV,
                                 uint32_t step) {
  if (maxV <= minV || step == 0) return minV;
  int32_t clamped = x;
  if (clamped < kSliderTrackX0) clamped = kSliderTrackX0;
  if (clamped > kSliderTrackX1) clamped = kSliderTrackX1;
  const uint32_t trackW =
      static_cast<uint32_t>(kSliderTrackX1 - kSliderTrackX0);
  const uint32_t offset = static_cast<uint32_t>(clamped - kSliderTrackX0);
  const uint32_t range = maxV - minV;
  const uint32_t steps = range / step;
  uint32_t index =
      static_cast<uint32_t>((static_cast<uint64_t>(offset) * steps +
                             trackW / 2) /
                            trackW);
  if (index > steps) index = steps;
  return minV + index * step;
}

inline int16_t sliderXFromValue(uint32_t value, uint32_t minV,
                                uint32_t maxV) {
  if (maxV <= minV) return kSliderTrackX0;
  if (value < minV) value = minV;
  if (value > maxV) value = maxV;
  const uint32_t trackW =
      static_cast<uint32_t>(kSliderTrackX1 - kSliderTrackX0);
  const uint32_t range = maxV - minV;
  return static_cast<int16_t>(
      kSliderTrackX0 +
      (static_cast<uint64_t>(value - minV) * trackW + range / 2) / range);
}

// Hit-tests the Display-tab slider rows. contentY is the tap position in
// content space (screenY + scrollOffset). Returns kNone for taps outside the
// slider rows; scrolling drags are handled by the controller.
inline Action hitTestDisplaySliders(int16_t x, int16_t contentY) {
  if (x < kSliderTrackX0 - kSliderThumbHalfW ||
      x > kSliderTrackX1 + kSliderThumbHalfW) {
    return Action();
  }
  if (contentY >= kSliderMinutesY - kSliderTouchHalfH &&
      contentY < kSliderMinutesY + kSliderTouchHalfH) {
    return Action(ActionKind::kSleepMinutes,
                  sliderValueFromX(x, 0,
                                   backlight_timer::kSleepMinutesMax, 1));
  }
  if (contentY >= kSliderHoursY - kSliderTouchHalfH &&
      contentY < kSliderHoursY + kSliderTouchHalfH) {
    return Action(ActionKind::kSleepHours,
                  sliderValueFromX(x, 0, backlight_timer::kSleepHoursMax,
                                   1));
  }
  if (contentY >= kSliderPollY - kSliderTouchHalfH &&
      contentY < kSliderPollY + kSliderTouchHalfH) {
    return Action(ActionKind::kPollInterval,
                  sliderValueFromX(x, backlight_timer::kPollSliderMinSec,
                                   backlight_timer::kPollSliderMaxSec,
                                   backlight_timer::kPollSliderStepSec));
  }
  return Action();
}

// Maps a screen Y on the scrollbar track to a scroll offset, placing the
// thumb center at the tap point. Used for drag scrolling.
inline int16_t displayScrollFromTrackY(int16_t y) {
  const int16_t trackH = kScrollBarY1 - kScrollBarY0;
  const int16_t thumbH = static_cast<int16_t>(
      (static_cast<int32_t>(kDisplayVisibleHeight) * trackH) /
      kDisplayContentHeight);
  const int16_t travel = trackH - thumbH;
  if (travel <= 0 || kDisplayScrollMax <= 0) {
    return 0;
  }
  const int32_t offset = (static_cast<int32_t>(y - thumbH / 2) -
                          kScrollBarY0) *
                         kDisplayScrollMax / travel;
  return clampScroll(offset);
}

inline Action hitTestDisplayTab(uint16_t x, uint16_t y, int16_t scroll) {
  const int16_t clamped = clampScroll(scroll);
  // Scrollbar first so it wins over the slider thumb end zone.
  if (x >= static_cast<uint16_t>(kScrollBarX0) && x < kDisplayWidth &&
      y >= kScrollBarY0 && y < kScrollBarY1) {
    const int16_t trackH = kScrollBarY1 - kScrollBarY0;
    const int16_t thumbH = static_cast<int16_t>(
        (static_cast<int32_t>(kDisplayVisibleHeight) * trackH) /
        kDisplayContentHeight);
    const int16_t travel = trackH - thumbH;
    const int16_t thumbY0 =
        travel <= 0 || kDisplayScrollMax <= 0
            ? kScrollBarY0
            : static_cast<int16_t>(
                  kScrollBarY0 +
                  (static_cast<int32_t>(clamped) * travel) /
                      kDisplayScrollMax);
    int16_t target = clamped;
    if (y < thumbY0) {
      target = clamped - kScrollPage;
    } else if (y >= thumbY0 + thumbH) {
      target = clamped + kScrollPage;
    } else {
      return Action();
    }
    return Action(ActionKind::kDisplayScroll,
                  static_cast<uint32_t>(clampScroll(target)));
  }
  if (y < kGridTop || y >= kFooterTop) {
    return Action();
  }
  return hitTestDisplaySliders(static_cast<int16_t>(x),
                               static_cast<int16_t>(y) + clamped);
}

inline Action hitTest(Tab activeTab, uint16_t x, uint16_t y,
                      int16_t scroll = 0) {
  if (y < kTabHeight) {
    return Action(
        x < kTabWidth ? ActionKind::kUsageTab : ActionKind::kDisplayTab, 0);
  }
  if (activeTab != Tab::kDisplay) {
    return Action();
  }
  return hitTestDisplayTab(x, y, scroll);
}

} // namespace touch_ui
