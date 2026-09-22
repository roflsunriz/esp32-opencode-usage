#pragma once

#include <cstddef>
#include <cstdint>

namespace backlight_timer {

constexpr uint32_t kDefaultTimeoutSec = 60;
constexpr uint32_t kTimeoutOptionsSec[] = {15,  30,   60,   120, 300,
                                           600, 1800, 3600, 7200};
constexpr size_t kTimeoutOptionCount =
    sizeof(kTimeoutOptionsSec) / sizeof(kTimeoutOptionsSec[0]);

// New Display-tab sliders (esp32-ui-style.md): 0-59 minutes + 0-24 hours.
// The total timeout is hours*3600 + minutes*60. 0 minutes + 0 hours disables
// the auto-off (always on). Legacy fixed choices (15s/30s/...) stay valid so
// stored records keep working; the sliders only produce minute-quantized
// values when the user drags them.
constexpr uint32_t kSleepMinutesMax = 59;
constexpr uint32_t kSleepHoursMax = 24;
constexpr uint32_t kSleepTimeoutMaxSec =
    kSleepHoursMax * 3600 + kSleepMinutesMax * 60; // 89940
// Poll-interval slider: 60-600 seconds in 60-second steps.
constexpr uint32_t kPollSliderMinSec = 60;
constexpr uint32_t kPollSliderMaxSec = 600;
constexpr uint32_t kPollSliderStepSec = 60;

inline bool isSupportedTimeout(uint32_t timeoutSec) {
  return timeoutSec <= kSleepTimeoutMaxSec;
}

inline uint32_t sleepMinutesPart(uint32_t timeoutSec) {
  return (timeoutSec % 3600) / 60;
}

inline uint32_t sleepHoursPart(uint32_t timeoutSec) {
  return timeoutSec / 3600;
}

inline uint32_t sleepTimeoutFromParts(uint32_t minutes, uint32_t hours) {
  if (minutes > kSleepMinutesMax) minutes = kSleepMinutesMax;
  if (hours > kSleepHoursMax) hours = kSleepHoursMax;
  return hours * 3600 + minutes * 60;
}

inline bool isValidPollSlider(uint32_t pollSec) {
  return pollSec >= kPollSliderMinSec && pollSec <= kPollSliderMaxSec &&
         pollSec % kPollSliderStepSec == 0;
}

// Pure state model shared by the ESP timer callback and native tests. All
// elapsed-time subtraction is unsigned so a millis() wrap keeps its meaning.
class Model {
public:
  void start(uint32_t timeoutSec, uint32_t nowMs) {
    timeoutSec_ =
        isSupportedTimeout(timeoutSec) ? timeoutSec : kDefaultTimeoutSec;
    lastActivityMs_ = nowMs;
    on_ = true;
  }

  // A configuration change is also activity. The return value says whether
  // it changed the visible power state.
  bool setTimeout(uint32_t timeoutSec, uint32_t nowMs) {
    if (!isSupportedTimeout(timeoutSec)) {
      return false;
    }
    const bool changed = !on_;
    timeoutSec_ = timeoutSec;
    lastActivityMs_ = nowMs;
    on_ = true;
    return changed;
  }

  // Touch and explicit wake reset the deadline even when the display is on.
  bool wake(uint32_t nowMs) {
    const bool changed = !on_;
    lastActivityMs_ = nowMs;
    on_ = true;
    return changed;
  }

  // Network/display updates intentionally do not call wake().
  bool tick(uint32_t nowMs) {
    if (!on_ || elapsedMs(nowMs) < timeoutMs()) {
      return false;
    }
    on_ = false;
    return true;
  }

  bool isOn() const { return on_; }
  uint32_t timeoutSec() const { return timeoutSec_; }

  uint32_t remainingMs(uint32_t nowMs) const {
    if (!on_) {
      return 0;
    }
    const uint32_t elapsed = elapsedMs(nowMs);
    const uint32_t timeout = timeoutMs();
    return elapsed >= timeout ? 0 : timeout - elapsed;
  }

private:
  uint32_t timeoutMs() const { return timeoutSec_ * 1000UL; }
  uint32_t elapsedMs(uint32_t nowMs) const { return nowMs - lastActivityMs_; }

  uint32_t timeoutSec_ = kDefaultTimeoutSec;
  uint32_t lastActivityMs_ = 0;
  bool on_ = true;
};

} // namespace backlight_timer
