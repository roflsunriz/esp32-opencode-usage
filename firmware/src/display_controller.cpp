#include "display_controller.h"

#include "board_pins.h"

#include <soc/gpio_struct.h>
#include <Preferences.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace {
constexpr uint16_t kBackground = 0x1082;
constexpr uint16_t kPanel = 0x18C3;
constexpr uint16_t kBarBackground = 0x4208;
constexpr uint16_t kActiveTab = ILI9341_CYAN;
constexpr uint16_t kInactiveTab = 0x4208;
constexpr uint16_t kSelectedButton = 0x05E0;
constexpr uint16_t kButton = 0x2965;
constexpr int16_t kFooterTop = touch_ui::kFooterTop;
constexpr int16_t kRowHeight = 62;
constexpr int16_t kFirstRowTop = touch_ui::kTabHeight + 2;
constexpr uint64_t kBacklightTimerPeriodUs = 10000;
constexpr uint32_t kTouchSpiFrequencyHz = 2000000;
constexpr uint8_t kEnablePenIrqCommand = 0x90;
constexpr int16_t kCapturePressure = 12;
constexpr uint32_t kCalibrationVersion = 2;
constexpr uint32_t kCalibrationVersionLegacy = 1;
struct StoredCalibration {
  uint32_t version;
  int16_t values[7];
  uint16_t check;
};
struct StoredCalibrationLegacy {
  uint32_t version;
  int16_t values[5];
  uint16_t check;
};
static_assert(sizeof(StoredCalibration) == 20, "touch calibration record size");
static_assert(sizeof(StoredCalibrationLegacy) == 16,
              "legacy touch calibration record size");
uint16_t calibrationCheck(const StoredCalibration& record) {
  uint16_t result = 0xA53C;
  for (const int16_t value : record.values) result ^= static_cast<uint16_t>(value);
  return result;
}
uint16_t calibrationCheckLegacy(const StoredCalibrationLegacy& record) {
  uint16_t result = 0xA53C;
  for (const int16_t value : record.values) result ^= static_cast<uint16_t>(value);
  return result;
}

uint16_t colorForPercent(double percent) {
  if (percent >= 90.0) {
    return ILI9341_RED;
  }
  if (percent >= 75.0) {
    return ILI9341_YELLOW;
  }
  return ILI9341_GREEN;
}

bool samePeriod(const usage_model::PeriodUsage &left,
                const usage_model::PeriodUsage &right) {
  return left.used == right.used && left.limit == right.limit &&
         left.percent == right.percent && left.resetInSec == right.resetInSec &&
         left.valid == right.valid;
}

bool sameSnapshot(const usage_model::UsageSnapshot &left,
                  const usage_model::UsageSnapshot &right) {
  // updatedAt is transport metadata and is not rendered. Avoid spending a
  // full display transfer on a poll that only refreshes that timestamp.
  return left.valid == right.valid && samePeriod(left.rolling, right.rolling) &&
         samePeriod(left.weekly, right.weekly) &&
         samePeriod(left.monthly, right.monthly);
}

} // namespace

DisplayController::DisplayController()
    : touchSpi_(VSPI), tft_(), canvas_(&tft_),
      touch_(board_pins::touch_cs, board_pins::touch_irq, 120) {
}

void DisplayController::begin(bool flipped) {
  pinMode(board_pins::tft_backlight, OUTPUT);
  digitalWrite(board_pins::tft_backlight, HIGH);
  tft_.init();
  setScreenFlipped(flipped);
  tft_.setTextWrap(false);
  canvas_.setColorDepth(8);
  canvasReady_ = canvas_.createSprite(touch_ui::kDisplayWidth,
                                      touch_ui::kDisplayHeight) != nullptr;
  canvas_.setTextWrap(false);
  presentScreen();

  backlight_.start(backlight_timer::kDefaultTimeoutSec, nowMs());
  pinMode(board_pins::boot_button, INPUT_PULLUP);
  bootButton_.start(digitalRead(board_pins::boot_button) == LOW, nowMs());
  esp_timer_create_args_t timerArgs = {};
  timerArgs.callback = &DisplayController::backlightTimerCallback;
  timerArgs.arg = this;
  timerArgs.dispatch_method = ESP_TIMER_TASK;
  timerArgs.name = "backlight";
  backlightTimerReady_ =
      esp_timer_create(&timerArgs, &backlightTimer_) == ESP_OK &&
      esp_timer_start_periodic(backlightTimer_, kBacklightTimerPeriodUs) ==
          ESP_OK;
  if (!backlightTimerReady_) {
    if (backlightTimer_ != nullptr) {
      esp_timer_delete(backlightTimer_);
      backlightTimer_ = nullptr;
    }
    showStatus("Backlight timer failed", ILI9341_RED);
  }
  armTouchInterrupt();
  loadCalibration();
  touch_.setPressureThreshold(touchCalibration_.pressure);
}

uint32_t DisplayController::nowMs() const {
  return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

void DisplayController::setScreenFlipped(bool flipped) {
  screenFlipped_ = flipped;
  tft_.setRotation(flipped ? 3 : 1);
  bandDiff_.invalidate();
}

bool DisplayController::consumeBootClick() {
  portENTER_CRITICAL(&backlightMux_);
  const bool clicked = pendingBootClicks_ != 0;
  if (clicked) {
    --pendingBootClicks_;
  }
  portEXIT_CRITICAL(&backlightMux_);
  return clicked;
}

bool DisplayController::consumeBootCalibration() {
  portENTER_CRITICAL(&backlightMux_);
  const bool requested = pendingBootCalibrations_ != 0;
  if (requested) --pendingBootCalibrations_;
  portEXIT_CRITICAL(&backlightMux_);
  return requested;
}

void DisplayController::redraw(const usage_model::UsageSnapshot *snapshot,
                                uint32_t timeoutSec,
                                uint32_t pollIntervalSec) {
  displayedTimeoutSec_ = timeoutSec;
  displayedPollSec_ = pollIntervalSec;
  if (snapshot != nullptr && snapshot->valid) {
    displayedSnapshot_ = *snapshot;
    hasDisplayedSnapshot_ = true;
  } else {
    hasDisplayedSnapshot_ = false;
  }
  // Rotation changes the LCD's GRAM coordinate system. Recompose every band
  // from retained state so the visible page and footer survive the flip.
  presentScreen();
}

void DisplayController::setBacklightPinLocked(bool on) {
  const uint32_t pinMask = 1UL << board_pins::tft_backlight;
  if (on) {
    GPIO.out_w1ts = pinMask;
  } else {
    GPIO.out_w1tc = pinMask;
  }
}

void DisplayController::backlightTimerCallback(void *context) {
  static_cast<DisplayController *>(context)->handleBacklightTimer();
}

void DisplayController::handleBacklightTimer() {
  const uint32_t currentMs = nowMs();
  portENTER_CRITICAL(&backlightMux_);
  if (bootButton_.update(digitalRead(board_pins::boot_button) == LOW,
                         currentMs)) {
    if (bootButton_.lastDurationMs() >= 1500) ++pendingBootCalibrations_;
    else ++pendingBootClicks_;
    if (backlight_.wake(currentMs)) {
      setBacklightPinLocked(true);
      backlightStateChanged_ = true;
    }
  }
  const bool changed = backlight_.tick(currentMs);
  if (changed) {
    setBacklightPinLocked(backlight_.isOn());
    backlightStateChanged_ = true;
  }
  portEXIT_CRITICAL(&backlightMux_);
}

void IRAM_ATTR DisplayController::touchInterrupt(void *context) {
  static_cast<DisplayController *>(context)->handleTouchInterrupt();
}

void IRAM_ATTR DisplayController::handleTouchInterrupt() {
  portENTER_CRITICAL_ISR(&backlightMux_);
  touchWakePending_ = true;
  touchReadPending_ = true;
  portEXIT_CRITICAL_ISR(&backlightMux_);
}

void DisplayController::armTouchInterrupt() {
  // The board schematic maps XPT2046 DCLK/CS/DIN/DOUT to 25/33/32/39 and
  // /PENIRQ to GPIO36. The XPT2046 datasheet requires a PD0=0 control byte
  // after a prior program left PD0=1, otherwise PENIRQ stays disabled. This
  // startup transaction discards conversion bytes; later taps use filtered
  // coordinate reads on the same verified SPI bus.
  touchSpi_.begin(board_pins::touch_sclk, board_pins::touch_miso,
                  board_pins::touch_mosi, board_pins::touch_cs);
  pinMode(board_pins::touch_cs, OUTPUT);
  digitalWrite(board_pins::touch_cs, HIGH);
  touchSpi_.beginTransaction(
      SPISettings(kTouchSpiFrequencyHz, MSBFIRST, SPI_MODE0));
  digitalWrite(board_pins::touch_cs, LOW);
  touchSpi_.transfer(kEnablePenIrqCommand);
  touchSpi_.transfer(0);
  touchSpi_.transfer(0);
  digitalWrite(board_pins::touch_cs, HIGH);
  touchSpi_.endTransaction();

  touch_.setInterruptCallback(&DisplayController::touchInterrupt, this);
  touch_.begin(touchSpi_);
  touch_.setRotation(1);
}

bool DisplayController::hasTouchPending() {
  portENTER_CRITICAL(&backlightMux_);
  const bool pending = touchReadPending_;
  portEXIT_CRITICAL(&backlightMux_);
  return pending;
}

bool DisplayController::readTouchPoint(touch_ui::Point &point) {
  if (!touch_.tirqTouched()) return false;
  int32_t sumX = 0, sumY = 0;
  for (int i = 0; i < 3; ++i) {
    const SensitiveTouchPoint sample = touch_.getPoint();
    if (sample.z < touchCalibration_.pressure ||
        !touch_ui::isPlausibleRaw(sample.x, sample.y)) return false;
    if (i && (abs(sample.x - sumX / i) > 220 ||
              abs(sample.y - sumY / i) > 220)) return false;
    sumX += sample.x; sumY += sample.y;
    delay(5);
  }
  const uint16_t rawX = static_cast<uint16_t>(sumX / 3);
  const uint16_t rawY = static_cast<uint16_t>(sumY / 3);
  point = touchCalibration_.configured
      ? touch_ui::mapPointWithMap(rawX, rawY, screenFlipped_,
                                  touchCalibration_.map)
      : touch_ui::mapPoint(rawX, rawY, screenFlipped_);
  return true;
}

void DisplayController::loadCalibration() {
  Preferences prefs;
  if (!prefs.begin("opencode-touch", true)) return;
  StoredCalibration stored = {};
  if (prefs.getBytesLength("calib") == sizeof(stored) &&
      prefs.getBytes("calib", &stored, sizeof(stored)) == sizeof(stored) &&
      stored.version == kCalibrationVersion &&
      stored.check == calibrationCheck(stored)) {
    TouchCalibration candidate;
    candidate.map.useRawXForX = stored.values[0] != 0;
    candidate.map.xStart = stored.values[1];
    candidate.map.xEnd = stored.values[2];
    candidate.map.useRawYForY = stored.values[3] != 0;
    candidate.map.yStart = stored.values[4];
    candidate.map.yEnd = stored.values[5];
    candidate.pressure = stored.values[6];
    candidate.configured = true;
    if (touch_ui::calibratedSpansValid(candidate.map) &&
        candidate.pressure >= kCapturePressure && candidate.pressure <= 120)
      touchCalibration_ = candidate;
    prefs.end();
    return;
  }
  StoredCalibrationLegacy legacy = {};
  if (prefs.getBytesLength("calib") == sizeof(legacy) &&
      prefs.getBytes("calib", &legacy, sizeof(legacy)) == sizeof(legacy) &&
      legacy.version == kCalibrationVersionLegacy &&
      legacy.check == calibrationCheckLegacy(legacy)) {
    TouchCalibration candidate;
    candidate.map = touch_ui::calibratedFromLegacy(
        legacy.values[0], legacy.values[1], legacy.values[2],
        legacy.values[3]);
    candidate.pressure = legacy.values[4];
    candidate.configured = true;
    if (touch_ui::calibratedSpansValid(candidate.map) &&
        candidate.pressure >= kCapturePressure && candidate.pressure <= 120)
      touchCalibration_ = candidate;
  }
  prefs.end();
}

bool DisplayController::saveCalibration() {
  Preferences prefs;
  if (!prefs.begin("opencode-touch", false)) return false;
  StoredCalibration stored = {kCalibrationVersion,
      {static_cast<int16_t>(touchCalibration_.map.useRawXForX ? 1 : 0),
       touchCalibration_.map.xStart, touchCalibration_.map.xEnd,
       static_cast<int16_t>(touchCalibration_.map.useRawYForY ? 1 : 0),
       touchCalibration_.map.yStart, touchCalibration_.map.yEnd,
       touchCalibration_.pressure}, 0};
  stored.check = calibrationCheck(stored);
  const bool saved = prefs.putBytes("calib", &stored, sizeof(stored)) == sizeof(stored);
  prefs.end();
  return saved;
}

bool DisplayController::captureCalibrationPoint(int16_t &rawX, int16_t &rawY,
                                                 int16_t &pressure) {
  const uint32_t start = nowMs();
  while (static_cast<uint32_t>(nowMs() - start) < 15000) {
    wakeBacklight();
    if (!touch_.tirqTouched()) { delay(10); continue; }
    int32_t sumX = 0, sumY = 0;
    int16_t count = 0, weakest = 32767;
    while (touch_.tirqTouched() && count < 12 &&
           static_cast<uint32_t>(nowMs() - start) < 15000) {
      wakeBacklight();
      const SensitiveTouchPoint point = touch_.getPoint();
      if (point.z >= kCapturePressure &&
          touch_ui::isPlausibleRaw(point.x, point.y)) {
        sumX += point.x; sumY += point.y;
        weakest = std::min(weakest, point.z);
        ++count;
      }
      delay(12);
    }
    while (touch_.tirqTouched() &&
           static_cast<uint32_t>(nowMs() - start) < 15000) {
      wakeBacklight();
      touch_.getPoint(); delay(10);
    }
    if (count >= 4) {
      rawX = static_cast<int16_t>(sumX / count);
      rawY = static_cast<int16_t>(sumY / count);
      pressure = weakest;
      return true;
    }
  }
  return false;
}

void DisplayController::calibrateTouch() {
  const bool flipped = screenFlipped_;
  tft_.setRotation(1);
  touch_.setPressureThreshold(kCapturePressure);
  auto step = [this](const char* label, int x, int y) {
    tft_.fillScreen(TFT_BLACK);
    tft_.setTextColor(TFT_WHITE, TFT_BLACK);
    tft_.drawString(label, 8, 8, 2);
    tft_.drawString("Press cross as usual", 8, 40, 2);
    tft_.fillRect(x - 10, y, 21, 1, TFT_YELLOW);
    tft_.fillRect(x, y - 10, 1, 21, TFT_YELLOW);
  };
  int16_t firstX = 0, firstY = 0, secondX = 0, secondY = 0;
  int16_t thirdX = 0, thirdY = 0;
  int16_t firstPressure = 0, secondPressure = 0, thirdPressure = 0;
  step("TOUCH 1/3", touch_ui::kCalibTargetX0, touch_ui::kCalibTargetY0);
  const bool first = captureCalibrationPoint(firstX, firstY, firstPressure);
  if (first) step("TOUCH 2/3", touch_ui::kCalibTargetX1, touch_ui::kCalibTargetY0);
  const bool second = first && captureCalibrationPoint(secondX, secondY,
                                                       secondPressure);
  if (second) step("TOUCH 3/3", touch_ui::kCalibTargetX0, touch_ui::kCalibTargetY1);
  const bool third = second && captureCalibrationPoint(thirdX, thirdY,
                                                       thirdPressure);
  const touch_ui::CalibratedMap measured = touch_ui::buildCalibratedMap(
      touch_ui::RawXY(firstX, firstY), touch_ui::RawXY(secondX, secondY),
      touch_ui::RawXY(thirdX, thirdY));
  if (third && touch_ui::calibratedSpansValid(measured)) {
    const TouchCalibration previous = touchCalibration_;
    touchCalibration_.map = measured;
    touchCalibration_.pressure = touch_ui::pressureThresholdFor(
        std::min({firstPressure, secondPressure, thirdPressure}));
    touchCalibration_.configured = true;
    if (!saveCalibration()) {
      touchCalibration_ = previous;
      tft_.fillScreen(TFT_BLACK);
      tft_.drawString("Calibration save failed", 8, 70, 2);
      delay(1800);
    }
  } else {
    tft_.fillScreen(TFT_BLACK);
    tft_.drawString(first ? "Calibration invalid" : "No touch detected", 8, 70, 2);
    delay(1800);
  }
  touch_.setPressureThreshold(touchCalibration_.pressure);
  tft_.setRotation(flipped ? 3 : 1);
  bandDiff_.invalidate();
  wakeBacklight();
  portENTER_CRITICAL(&backlightMux_);
  touchReadPending_ = touchWakePending_ = false;
  portEXIT_CRITICAL(&backlightMux_);
  touchLatch_ = touch_ui::ReleaseLatch();
}

bool DisplayController::pollTouch(TouchEvent &event) {
  event = TouchEvent();
  const uint32_t currentMs = nowMs();
  const bool penLow = digitalRead(board_pins::touch_irq) == LOW;
  touchLatch_.update(penLow, currentMs);
  portENTER_CRITICAL(&backlightMux_);
  const bool pending = touchReadPending_;
  touchReadPending_ = false;
  touchWakePending_ = false;
  portEXIT_CRITICAL(&backlightMux_);
  if (!pending) {
    // Drag continuation: while a contact persists on the Display tab,
    // report slider value changes without waiting for release, so a pen
    // pressed against the screen can drag a slider. Only quantized value
    // changes are reported, and the latch still guards the next contact.
    if (!penLow) {
      lastDragKind_ = touch_ui::ActionKind::kNone;
    } else if (touchLatch_.isLatched() && backlightOn() &&
               activeTab_ == touch_ui::Tab::kDisplay) {
      touch_ui::Point dragPoint;
      if (readTouchPoint(dragPoint)) {
        wakeBacklight();
        const touch_ui::Action dragAction = touch_ui::hitTest(
            activeTab_, dragPoint.x, dragPoint.y, displayScroll_);
        if ((dragAction.kind == touch_ui::ActionKind::kSleepMinutes ||
             dragAction.kind == touch_ui::ActionKind::kSleepHours ||
             dragAction.kind == touch_ui::ActionKind::kPollInterval) &&
            (dragAction.kind != lastDragKind_ ||
             dragAction.timeoutSec != lastDragValue_)) {
          lastDragKind_ = dragAction.kind;
          lastDragValue_ = dragAction.timeoutSec;
          event.kind = TouchEventKind::kTap;
          event.hasCoordinates = true;
          event.point = dragPoint;
          event.action = dragAction;
          return true;
        }
      }
    }
    return false;
  }
  if (!touchLatch_.accept()) {
    return false;
  }
  if (!penLow) {
    // PENIRQ glitch (for example a conversion transient) with no finger
    // present. Stay silent so noise cannot wake the backlight, defer the
    // network, or spam touch diagnostics.
    return false;
  }
  portENTER_CRITICAL(&backlightMux_);
  const bool wasOff = !backlight_.isOn();
  portEXIT_CRITICAL(&backlightMux_);
  if (!readTouchPoint(event.point)) {
    // A real contact that failed validation still wakes a sleeping screen,
    // but it is not a tap.
    if (wasOff) {
      wakeBacklight();
    }
    return true;
  }
  wakeBacklight();
  if (wasOff) {
    event.kind = TouchEventKind::kWakeOnly;
    return true;
  }
  event.kind = TouchEventKind::kTap;
  event.hasCoordinates = true;
  event.action = touch_ui::hitTest(activeTab_, event.point.x, event.point.y,
                                   displayScroll_);
  if (event.action.kind == touch_ui::ActionKind::kSleepMinutes ||
      event.action.kind == touch_ui::ActionKind::kSleepHours ||
      event.action.kind == touch_ui::ActionKind::kPollInterval) {
    lastDragKind_ = event.action.kind;
    lastDragValue_ = event.action.timeoutSec;
  } else {
    lastDragKind_ = touch_ui::ActionKind::kNone;
  }
  return true;
}

bool DisplayController::setBacklightTimeoutSec(uint32_t timeoutSec) {
  if (!backlight_timer::isSupportedTimeout(timeoutSec)) {
    return false;
  }
  const uint32_t currentMs = nowMs();
  portENTER_CRITICAL(&backlightMux_);
  const bool changed = backlight_.setTimeout(timeoutSec, currentMs);
  if (changed) {
    setBacklightPinLocked(true);
    backlightStateChanged_ = true;
  }
  portEXIT_CRITICAL(&backlightMux_);
  return true;
}

void DisplayController::wakeBacklight() {
  const uint32_t currentMs = nowMs();
  portENTER_CRITICAL(&backlightMux_);
  if (backlight_.wake(currentMs)) {
    setBacklightPinLocked(true);
    backlightStateChanged_ = true;
  }
  portEXIT_CRITICAL(&backlightMux_);
}

bool DisplayController::consumeBacklightStateChanged() {
  portENTER_CRITICAL(&backlightMux_);
  const bool changed = backlightStateChanged_;
  backlightStateChanged_ = false;
  portEXIT_CRITICAL(&backlightMux_);
  return changed;
}

bool DisplayController::backlightOn() {
  portENTER_CRITICAL(&backlightMux_);
  const bool on = backlight_.isOn();
  portEXIT_CRITICAL(&backlightMux_);
  return on;
}

uint32_t DisplayController::backlightTimeoutSec() {
  portENTER_CRITICAL(&backlightMux_);
  const uint32_t timeoutSec = backlight_.timeoutSec();
  portEXIT_CRITICAL(&backlightMux_);
  return timeoutSec;
}

uint32_t DisplayController::backlightRemainingMs() {
  const uint32_t currentMs = nowMs();
  portENTER_CRITICAL(&backlightMux_);
  const uint32_t remainingMs = backlight_.remainingMs(currentMs);
  portEXIT_CRITICAL(&backlightMux_);
  return remainingMs;
}

int DisplayController::backlightGpioLevel() const {
  return digitalRead(board_pins::tft_backlight);
}

void DisplayController::presentScreen() {
  TFT_eSPI &surface = canvasReady_ ? static_cast<TFT_eSPI &>(canvas_) : tft_;
  surface.setTextFont(1);
  drawScreen(surface);
  if (canvasReady_) {
    const uint16_t changed = bandDiff_.update(
        static_cast<const uint8_t*>(canvas_.getPointer()));
    if (!display_diff::eachRun(changed, [this](size_t top, size_t height) {
          return canvas_.pushSprite(0, static_cast<int32_t>(top), 0,
                                    static_cast<int32_t>(top),
                                    static_cast<int32_t>(display_diff::kWidth),
                                    static_cast<int32_t>(height));
        })) {
      canvas_.pushSprite(0, 0);
      bandDiff_.invalidate();
    }
  }
  hasPresentedScreen_ = true;
}

void DisplayController::presentStatusBand() {
  presentScreen();
}

void DisplayController::drawScreen(TFT_eSPI &surface) {
  display_diff::clearFrame(surface, kBackground);
  drawHeader(surface);
  if (activeTab_ == touch_ui::Tab::kDisplay) {
    drawDisplaySettings(surface, displayedTimeoutSec_);
  } else {
    drawUsage(surface);
  }
  drawStatus(surface);
}

void DisplayController::drawHeader(TFT_eSPI &surface) {
  const bool usageActive = activeTab_ == touch_ui::Tab::kUsage;
  surface.fillRect(0, 0, touch_ui::kDisplayWidth, touch_ui::kTabHeight,
                   ILI9341_BLACK);
  surface.fillRect(0, 0, touch_ui::kTabWidth, touch_ui::kTabHeight,
                   usageActive ? kActiveTab : kInactiveTab);
  surface.fillRect(touch_ui::kTabWidth, 0, touch_ui::kTabWidth,
                   touch_ui::kTabHeight,
                   usageActive ? kInactiveTab : kActiveTab);
  surface.drawFastHLine(0, touch_ui::kTabHeight - 1,
                        touch_ui::kDisplayWidth, ILI9341_WHITE);
  surface.setTextColor(usageActive ? ILI9341_BLACK : ILI9341_WHITE,
                       usageActive ? kActiveTab : kInactiveTab);
  surface.setTextSize(2);
  surface.setCursor(32, 12);
  surface.print("Usage");
  surface.setTextColor(usageActive ? ILI9341_WHITE : ILI9341_BLACK,
                       usageActive ? kInactiveTab : kActiveTab);
  surface.setCursor(180, 12);
  surface.print("Display");
}

void DisplayController::drawDisplaySettings(TFT_eSPI &surface,
                                             uint32_t selectedTimeoutSec) {
  const uint32_t minutes =
      backlight_timer::sleepMinutesPart(selectedTimeoutSec);
  const uint32_t hours = backlight_timer::sleepHoursPart(selectedTimeoutSec);
  const int16_t scroll = touch_ui::clampScroll(displayScroll_);
  auto contentY = [scroll](int16_t y) -> int16_t {
    return static_cast<int16_t>(y - scroll);
  };
  auto drawSlider = [&](int16_t centerY, uint32_t value, uint32_t minV,
                        uint32_t maxV) {
    const int16_t y = contentY(centerY);
    if (y < touch_ui::kTabHeight - 8 ||
        y > touch_ui::kFooterTop + 8) {
      return;
    }
    surface.drawRect(touch_ui::kSliderTrackX0, y - 2,
                     touch_ui::kSliderTrackX1 - touch_ui::kSliderTrackX0,
                     5, ILI9341_WHITE);
    const int16_t thumbX = touch_ui::sliderXFromValue(value, minV, maxV);
    surface.fillRect(
        touch_ui::kSliderTrackX0, y - 2,
        static_cast<int16_t>(thumbX - touch_ui::kSliderTrackX0), 5,
        kActiveTab);
    surface.fillRect(thumbX - 6, y - 8, 12, 17, ILI9341_WHITE);
    surface.fillRect(thumbX - 4, y - 6, 8, 13, kButton);
  };
  auto drawLine = [&](int16_t y, const char *text, uint16_t color) {
    const int16_t visible = contentY(y);
    if (visible < touch_ui::kTabHeight ||
        visible > touch_ui::kFooterTop - 10) {
      return;
    }
    drawTextClipped(surface, text, 6, visible, 1, color,
                    touch_ui::kDisplayWidth - 30);
  };
  char line[56] = {};
  if (selectedTimeoutSec == 0) {
    snprintf(line, sizeof(line), "Off after: always on");
  } else {
    snprintf(line, sizeof(line), "Off after: %uh %02um", hours, minutes);
  }
  drawLine(46, line, ILI9341_WHITE);
  snprintf(line, sizeof(line), "Minutes 0-59: %um", minutes);
  drawLine(64, line, ILI9341_LIGHTGREY);
  drawSlider(touch_ui::kSliderMinutesY, minutes, 0,
             backlight_timer::kSleepMinutesMax);
  snprintf(line, sizeof(line), "Hours 0-24: %uh", hours);
  drawLine(114, line, ILI9341_LIGHTGREY);
  drawSlider(touch_ui::kSliderHoursY, hours, 0,
             backlight_timer::kSleepHoursMax);
  snprintf(line, sizeof(line), "Update 60-600s: %us", displayedPollSec_);
  drawLine(164, line, ILI9341_LIGHTGREY);
  drawSlider(touch_ui::kSliderPollY, displayedPollSec_,
             backlight_timer::kPollSliderMinSec,
             backlight_timer::kPollSliderMaxSec);
  drawLine(196, "0m 0h = always on", ILI9341_LIGHTGREY);
  drawLine(208, "Tap a track to set", ILI9341_LIGHTGREY);
  // Scrollbar on the right edge.
  const int16_t trackH =
      touch_ui::kScrollBarY1 - touch_ui::kScrollBarY0;
  surface.drawRect(touch_ui::kScrollBarX0, touch_ui::kScrollBarY0, 12,
                   trackH, kBarBackground);
  const int16_t thumbH = static_cast<int16_t>(
      (static_cast<int32_t>(touch_ui::kDisplayVisibleHeight) * trackH) /
      touch_ui::kDisplayContentHeight);
  const int16_t travel = trackH - thumbH;
  const int16_t thumbY0 =
      travel <= 0 || touch_ui::kDisplayScrollMax <= 0
          ? touch_ui::kScrollBarY0
          : static_cast<int16_t>(
                touch_ui::kScrollBarY0 +
                (static_cast<int32_t>(scroll) * travel) /
                    touch_ui::kDisplayScrollMax);
  surface.fillRect(touch_ui::kScrollBarX0 + 2, thumbY0, 8, thumbH,
                   ILI9341_WHITE);
}

void DisplayController::drawUsage(TFT_eSPI &surface) {
  const usage_model::PeriodUsage empty;
  const usage_model::UsageSnapshot *snapshot =
      hasDisplayedSnapshot_ ? &displayedSnapshot_ : nullptr;
  drawPeriod(surface, "5H", snapshot == nullptr ? empty : snapshot->rolling,
             kFirstRowTop);
  drawPeriod(surface, "WEEK", snapshot == nullptr ? empty : snapshot->weekly,
             kFirstRowTop + kRowHeight);
  drawPeriod(surface, "MONTH", snapshot == nullptr ? empty : snapshot->monthly,
             kFirstRowTop + kRowHeight * 2);
}

void DisplayController::drawStatus(TFT_eSPI &surface) {
  surface.fillRect(0, kFooterTop, touch_ui::kDisplayWidth,
                   touch_ui::kDisplayHeight - kFooterTop, kBackground);
  char composed[96] = {};
  snprintf(composed, sizeof(composed), "%s", status_);
  if (pollCountdownSec_ != UINT32_MAX) {
    char suffix[24] = {};
    if (pollCountdownSec_ == 0) {
      snprintf(suffix, sizeof(suffix), " - polling...");
    } else {
      snprintf(suffix, sizeof(suffix), " - next %us", pollCountdownSec_);
    }
    strncat(composed, suffix, sizeof(composed) - strlen(composed) - 1);
  }
  drawTextClipped(surface, canvasReady_ ? composed : "Display memory low - reboot",
                  5, kFooterTop + 2, 1,
                  canvasReady_ ? statusColor_ : ILI9341_RED,
                  touch_ui::kDisplayWidth - 10);
}

void DisplayController::drawTextClipped(TFT_eSPI &surface,
                                        const char *message, int16_t x,
                                        int16_t y, uint8_t textSize,
                                        uint16_t color, uint16_t maxWidth) {
  if (message == nullptr) {
    return;
  }
  const size_t maxChars = maxWidth / (6U * textSize);
  char clipped[80] = {};
  const size_t messageLength = strlen(message);
  const size_t copyLength =
      messageLength < sizeof(clipped) - 1 ? messageLength : sizeof(clipped) - 1;
  memcpy(clipped, message, copyLength);
  clipped[copyLength] = '\0';
  if (strlen(clipped) > maxChars && maxChars > 3) {
    clipped[maxChars - 3] = '.';
    clipped[maxChars - 2] = '.';
    clipped[maxChars - 1] = '.';
    clipped[maxChars] = '\0';
  }
  surface.setTextColor(color, kBackground);
  surface.setTextSize(textSize);
  surface.setCursor(x, y);
  surface.print(clipped);
}

void DisplayController::drawReset(TFT_eSPI &surface, int16_t x, int16_t y,
                                  int64_t resetInSec) {
  char resetText[32] = {};
  if (resetInSec < 0) {
    snprintf(resetText, sizeof(resetText), "reset --");
  } else if (resetInSec >= 86400) {
    snprintf(resetText, sizeof(resetText), "reset %lldd %02lldh",
             static_cast<long long>(resetInSec / 86400),
             static_cast<long long>((resetInSec / 3600) % 24));
  } else {
    snprintf(resetText, sizeof(resetText), "reset %02lldh %02lldm",
             static_cast<long long>(resetInSec / 3600),
             static_cast<long long>((resetInSec / 60) % 60));
  }
  drawTextClipped(surface, resetText, x, y, 1, ILI9341_LIGHTGREY, 130);
}

void DisplayController::drawPeriod(TFT_eSPI &surface, const char *label,
                                   const usage_model::PeriodUsage &period,
                                   int16_t top) {
  surface.fillRoundRect(2, top, touch_ui::kDisplayWidth - 4, kRowHeight - 3,
                        3, kPanel);
  surface.setTextColor(ILI9341_WHITE, kPanel);
  surface.setTextSize(2);
  surface.setCursor(7, top + 4);
  surface.print(label);

  if (!period.valid) {
    surface.setTextSize(1);
    surface.setTextColor(ILI9341_LIGHTGREY, kPanel);
    surface.setCursor(64, top + 9);
    surface.print("waiting for valid data");
    surface.drawRect(5, top + 27, touch_ui::kDisplayWidth - 10, 13,
                     kBarBackground);
    drawReset(surface, 7, top + 46, -1);
    return;
  }

  char amountText[48] = {};
  snprintf(amountText, sizeof(amountText), "$%.2f / $%.2f", period.used,
           period.limit);
  // Keep the percentage in a fixed right-hand column so long dollar values
  // cannot hide the primary percentage indicator.
  drawTextClipped(surface, amountText, 74, top + 8, 1, ILI9341_WHITE, 176);
  char percentText[16] = {};
  snprintf(percentText, sizeof(percentText), "%5.1f%%", period.percent);
  drawTextClipped(surface, percentText, 254, top + 8, 1,
                  colorForPercent(period.percent), 60);

  constexpr uint16_t kBarWidth = 306;
  constexpr uint16_t kBarHeight = 11;
  surface.drawRect(5, top + 27, kBarWidth + 2, kBarHeight + 2,
                   ILI9341_WHITE);
  surface.fillRect(7, top + 29, kBarWidth, kBarHeight, kBarBackground);
  const uint16_t fill = usage_model::filledWidth(period.percent, kBarWidth);
  if (fill > 0) {
    surface.fillRect(7, top + 29, fill, kBarHeight,
                     colorForPercent(period.percent));
  }
  drawReset(surface, 7, top + 46, period.resetInSec);
}

bool DisplayController::updateStatus(const char *message, uint16_t color) {
  char nextStatus[sizeof(status_)] = {};
  if (message != nullptr) {
    // Do not use snprintf(status_, ..., status_): overlapping source and
    // destination is undefined. A temporary also handles a pointer into the
    // current status buffer safely.
    snprintf(nextStatus, sizeof(nextStatus), "%s", message);
  } else {
    memcpy(nextStatus, status_, sizeof(nextStatus));
  }
  const bool textChanged = strcmp(status_, nextStatus) != 0;
  const bool colorChanged = statusColor_ != color;
  if (textChanged) {
    memcpy(status_, nextStatus, sizeof(status_));
  }
  statusColor_ = color;
  return textChanged || colorChanged;
}

bool DisplayController::updateSnapshot(
    const usage_model::UsageSnapshot &snapshot) {
  const bool changed =
      !hasDisplayedSnapshot_ || !sameSnapshot(displayedSnapshot_, snapshot);
  displayedSnapshot_ = snapshot;
  hasDisplayedSnapshot_ = true;
  return changed;
}

void DisplayController::showStatus(const char *message, uint16_t color) {
  if (!updateStatus(message, color)) {
    return;
  }
  if (hasPresentedScreen_) {
    presentStatusBand();
  }
}

void DisplayController::renderNoData(const char *message) {
  if (activeTab_ != touch_ui::Tab::kUsage) {
    return;
  }
  const bool contentChanged = hasDisplayedSnapshot_;
  hasDisplayedSnapshot_ = false;
  const bool statusChanged = updateStatus(message, ILI9341_YELLOW);
  if (!hasPresentedScreen_ || contentChanged) {
    presentScreen();
  } else if (statusChanged) {
    presentStatusBand();
  }
}

void DisplayController::renderSnapshot(
    const usage_model::UsageSnapshot &snapshot) {
  const bool contentChanged = updateSnapshot(snapshot);
  if (activeTab_ != touch_ui::Tab::kUsage) {
    return;
  }
  const bool statusChanged = updateStatus("Usage updated", ILI9341_GREEN);
  if (!hasPresentedScreen_ || contentChanged) {
    presentScreen();
  } else if (statusChanged) {
    presentStatusBand();
  }
}

void DisplayController::showUsageTab() {
  if (activeTab_ == touch_ui::Tab::kUsage) {
    return;
  }
  activeTab_ = touch_ui::Tab::kUsage;
  presentScreen();
}

void DisplayController::showDisplayTab(uint32_t selectedTimeoutSec,
                                        uint32_t pollIntervalSec) {
  const bool changed = activeTab_ != touch_ui::Tab::kDisplay ||
                       displayedTimeoutSec_ != selectedTimeoutSec ||
                       displayedPollSec_ != pollIntervalSec;
  activeTab_ = touch_ui::Tab::kDisplay;
  displayedTimeoutSec_ = selectedTimeoutSec;
  displayedPollSec_ = pollIntervalSec;
  displayScroll_ = touch_ui::clampScroll(displayScroll_);
  if (!hasPresentedScreen_ || changed) {
    presentScreen();
  }
}

void DisplayController::setDisplayScroll(int16_t scroll) {
  const int16_t clamped = touch_ui::clampScroll(scroll);
  if (clamped == displayScroll_) {
    return;
  }
  displayScroll_ = clamped;
  if (hasPresentedScreen_ && activeTab_ == touch_ui::Tab::kDisplay) {
    presentScreen();
  }
}

bool DisplayController::updatePollCountdown(uint32_t remainingMs,
                                            uint32_t pollIntervalSec) {
  displayedPollSec_ = pollIntervalSec;
  const uint32_t seconds =
      remainingMs == UINT32_MAX ? UINT32_MAX : (remainingMs + 999) / 1000;
  if (seconds == pollCountdownSec_) {
    return false;
  }
  pollCountdownSec_ = seconds;
  if (hasPresentedScreen_ && activeTab_ == touch_ui::Tab::kUsage) {
    presentScreen();
  }
  return true;
}

// Diagnostic readback from the real ILI9341 GRAM. A row buffer avoids
// allocating a second 150 KB framebuffer. RAMRD returns one dummy byte then
// RGB666 bytes.
void DisplayController::capture() {
  Serial.println("{\"type\":\"screenshot\",\"width\":320,\"height\":240,"
                 "\"format\":\"rgb888\",\"bytes\":230400}");
  Serial.flush();
  // Read in a fixed board orientation so diagnostics show the actual flip,
  // rather than silently normalizing both rotations to an upright image.
  tft_.setRotation(1);
  uint8_t line[960];
  for (int y = 0; y < 240; y++) {
    tft_.readRectRGB(0, y, 320, 1, line);
    Serial.write(line, sizeof(line));
    yield();
  }
  tft_.setRotation(screenFlipped_ ? 3 : 1);
  Serial.println();
}
