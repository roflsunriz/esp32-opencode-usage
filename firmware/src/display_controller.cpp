#include "display_controller.h"

#include "banded_frame.h"
#include "board_pins.h"

#include <soc/gpio_struct.h>

#include <cstdio>
#include <cstring>

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
// Adafruit's TSC2046 driver uses a conservative 2 MHz SPI default. It is
// below the XPT2046 timing limit and avoids board-to-board read noise.
constexpr uint32_t kTouchSpiFrequencyHz = 2000000;
constexpr uint8_t kEnablePenIrqCommand = 0x90;
constexpr uint8_t kReadXCommand = 0xD0;
constexpr uint8_t kReadYCommand = 0x90;
constexpr size_t kTouchSampleCount = 5;

// Adafruit_GFX normally owns a full canvas. This keeps its 320x240 logical
// coordinate system, while storing only the currently transferred 16-line
// band. Text and rounded primitives are clipped at the final pixel write, so
// each SPI transfer contains a complete image with no erase-before-draw pass.
class BandCanvas16 final : public Adafruit_GFX {
public:
  BandCanvas16(uint16_t *pixels, int16_t width, int16_t height,
               int16_t bandHeight)
      : Adafruit_GFX(width, height), pixels_(pixels), bandHeight_(bandHeight) {
    setTextWrap(false);
  }

  void setBandTop(int16_t top) { bandTop_ = top; }

  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    if (x < 0 || x >= _width || y < bandTop_ ||
        y >= bandTop_ + bandHeight_) {
      return;
    }
    pixels_[static_cast<size_t>(y - bandTop_) * _width + x] = color;
  }

  void drawFastVLine(int16_t x, int16_t y, int16_t height,
                     uint16_t color) override {
    fillRect(x, y, 1, height, color);
  }

  void drawFastHLine(int16_t x, int16_t y, int16_t width,
                     uint16_t color) override {
    fillRect(x, y, width, 1, color);
  }

  void fillRect(int16_t x, int16_t y, int16_t width, int16_t height,
                uint16_t color) override {
    const banded_frame::Rect area =
        banded_frame::clipToBand(x, y, width, height, _width, _height,
                                 bandTop_, bandHeight_);
    if (!area.visible) {
      return;
    }
    for (int16_t row = 0; row < area.height; ++row) {
      uint16_t *destination =
          pixels_ + static_cast<size_t>(area.y - bandTop_ + row) * _width +
          area.x;
      for (int16_t column = 0; column < area.width; ++column) {
        destination[column] = color;
      }
    }
  }

  void fillScreen(uint16_t color) override {
    for (size_t index = 0;
         index < static_cast<size_t>(_width) * bandHeight_; ++index) {
      pixels_[index] = color;
    }
  }

private:
  uint16_t *pixels_ = nullptr;
  int16_t bandTop_ = 0;
  int16_t bandHeight_ = 0;
};

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
    : spi_(VSPI), touchSpi_(HSPI),
      tft_(&spi_, board_pins::tft_dc, board_pins::tft_cs, board_pins::tft_rst) {
}

void DisplayController::begin(bool flipped) {
  pinMode(board_pins::tft_backlight, OUTPUT);
  digitalWrite(board_pins::tft_backlight, HIGH);
  spi_.begin(board_pins::tft_sclk, board_pins::tft_miso, board_pins::tft_mosi,
             board_pins::tft_cs);
  tft_.begin(40000000);
  setScreenFlipped(flipped);
  tft_.setTextWrap(false);
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
}

uint32_t DisplayController::nowMs() const {
  return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

void DisplayController::setScreenFlipped(bool flipped) {
  screenFlipped_ = flipped;
  tft_.setRotation(flipped ? 3 : 1);
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

void DisplayController::redraw(const usage_model::UsageSnapshot *snapshot,
                               uint32_t timeoutSec) {
  displayedTimeoutSec_ = timeoutSec;
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
    ++pendingBootClicks_;
    if (backlight_.wake(currentMs)) {
      setBacklightPinLocked(true);
      backlightStateChanged_ = true;
    }
  }
  const bool touchHandled = wakeForTouchLocked(currentMs);
  const bool changed = touchHandled ? false : backlight_.tick(currentMs);
  if (changed) {
    setBacklightPinLocked(backlight_.isOn());
    backlightStateChanged_ = true;
  }
  portEXIT_CRITICAL(&backlightMux_);
}

bool DisplayController::wakeForTouchLocked(uint32_t currentMs) {
  if (!touchWakePending_) {
    return false;
  }
  touchWakePending_ = false;
  const bool wakeOnly = !backlight_.isOn();
  if (backlight_.wake(currentMs)) {
    setBacklightPinLocked(true);
    backlightStateChanged_ = true;
  }
  touchWakeOnly_ = touchWakeOnly_ || wakeOnly;
  return true;
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

  pinMode(board_pins::touch_irq, INPUT);
  attachInterruptArg(board_pins::touch_irq, &DisplayController::touchInterrupt,
                     this, FALLING);
}

bool DisplayController::hasTouchPending() {
  portENTER_CRITICAL(&backlightMux_);
  const bool pending = touchReadPending_;
  portEXIT_CRITICAL(&backlightMux_);
  return pending;
}

uint16_t DisplayController::readTouchCoordinate(uint8_t command) {
  touchSpi_.transfer(command);
  const uint16_t upper = touchSpi_.transfer(0);
  const uint16_t lower = touchSpi_.transfer(0);
  return static_cast<uint16_t>(((upper << 8) | lower) >> 3);
}

bool DisplayController::readTouchPoint(touch_ui::Point &point) {
  if (digitalRead(board_pins::touch_irq) != LOW) {
    return false;
  }

  uint16_t rawX[kTouchSampleCount] = {};
  uint16_t rawY[kTouchSampleCount] = {};
  touchSpi_.beginTransaction(
      SPISettings(kTouchSpiFrequencyHz, MSBFIRST, SPI_MODE0));
  digitalWrite(board_pins::touch_cs, LOW);
  for (size_t index = 0; index < kTouchSampleCount; ++index) {
    rawX[index] = readTouchCoordinate(kReadXCommand);
    rawY[index] = readTouchCoordinate(kReadYCommand);
  }
  digitalWrite(board_pins::touch_cs, HIGH);
  touchSpi_.endTransaction();

  const uint16_t medianX = touch_ui::median5(rawX);
  const uint16_t medianY = touch_ui::median5(rawY);
  if (!touch_ui::isPlausibleRaw(medianX, medianY)) {
    return false;
  }
  point = touch_ui::mapPoint(medianX, medianY, screenFlipped_);
  return true;
}

bool DisplayController::pollTouch(TouchEvent &event) {
  event = TouchEvent();
  const uint32_t currentMs = nowMs();
  touchLatch_.update(digitalRead(board_pins::touch_irq) == LOW, currentMs);
  portENTER_CRITICAL(&backlightMux_);
  const bool pending = touchReadPending_;
  touchReadPending_ = false;
  wakeForTouchLocked(currentMs);
  const bool wakeOnly = touchWakeOnly_;
  touchWakeOnly_ = false;
  portEXIT_CRITICAL(&backlightMux_);
  if (!pending) {
    return false;
  }
  if (!touchLatch_.accept()) {
    return false;
  }
  if (wakeOnly) {
    event.kind = TouchEventKind::kWakeOnly;
    return true;
  }
  if (!readTouchPoint(event.point)) {
    return true;
  }
  event.kind = TouchEventKind::kTap;
  event.hasCoordinates = true;
  event.action = touch_ui::hitTest(activeTab_, event.point.x, event.point.y);
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
  tft_.startWrite();
  for (uint16_t top = 0; top < touch_ui::kDisplayHeight;
       top += kFrameBandHeight) {
    BandCanvas16 canvas(frameBand_, touch_ui::kDisplayWidth,
                        touch_ui::kDisplayHeight, kFrameBandHeight);
    canvas.setBandTop(top);
    drawScreen(canvas);
    tft_.setAddrWindow(0, top, touch_ui::kDisplayWidth, kFrameBandHeight);
    tft_.writePixels(frameBand_,
                     touch_ui::kDisplayWidth * kFrameBandHeight);
    yield();
  }
  tft_.endWrite();
  hasPresentedScreen_ = true;
}

void DisplayController::presentStatusBand() {
  const uint16_t top =
      (static_cast<uint16_t>(kFooterTop) / kFrameBandHeight) * kFrameBandHeight;
  presentBand(top);
}

void DisplayController::presentBand(uint16_t top) {
  BandCanvas16 canvas(frameBand_, touch_ui::kDisplayWidth,
                      touch_ui::kDisplayHeight, kFrameBandHeight);
  canvas.setBandTop(top);
  drawScreen(canvas);
  tft_.startWrite();
  tft_.setAddrWindow(0, top, touch_ui::kDisplayWidth, kFrameBandHeight);
  tft_.writePixels(frameBand_, touch_ui::kDisplayWidth * kFrameBandHeight);
  tft_.endWrite();
  hasPresentedScreen_ = true;
}

void DisplayController::drawScreen(Adafruit_GFX &surface) {
  surface.fillScreen(kBackground);
  drawHeader(surface);
  if (activeTab_ == touch_ui::Tab::kDisplay) {
    drawDisplaySettings(surface, displayedTimeoutSec_);
  } else {
    drawUsage(surface);
  }
  drawStatus(surface);
}

void DisplayController::drawHeader(Adafruit_GFX &surface) {
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

void DisplayController::drawDisplaySettings(Adafruit_GFX &surface,
                                            uint32_t selectedTimeoutSec) {
  for (uint16_t row = 0; row < touch_ui::kGridRows; ++row) {
    for (uint16_t column = 0; column < touch_ui::kGridColumns; ++column) {
      const size_t index = row * touch_ui::kGridColumns + column;
      const uint32_t timeoutSec = backlight_timer::kTimeoutOptionsSec[index];
      const int16_t left =
          touch_ui::kGridLeft +
          column * (touch_ui::kButtonWidth + touch_ui::kGridGapX);
      const int16_t top = touch_ui::kGridTop +
                          row * (touch_ui::kButtonHeight + touch_ui::kGridGapY);
      const bool selected = timeoutSec == selectedTimeoutSec;
      const uint16_t color = selected ? kSelectedButton : kButton;
      surface.fillRoundRect(left, top, touch_ui::kButtonWidth,
                            touch_ui::kButtonHeight, 5, color);
      surface.drawRoundRect(left, top, touch_ui::kButtonWidth,
                            touch_ui::kButtonHeight, 5, ILI9341_WHITE);
      const char *label = touch_ui::timeoutLabel(timeoutSec);
      surface.setTextColor(ILI9341_WHITE, color);
      surface.setTextSize(2);
      const int16_t textWidth = static_cast<int16_t>(strlen(label) * 12);
      surface.setCursor(left + (touch_ui::kButtonWidth - textWidth) / 2,
                        top + 17);
      surface.print(label);
    }
  }
  surface.setTextColor(ILI9341_LIGHTGREY, kBackground);
  surface.setTextSize(1);
  surface.setCursor(6, 215);
  surface.print("Selected: ");
  surface.print(touch_ui::timeoutLabel(selectedTimeoutSec));
}

void DisplayController::drawUsage(Adafruit_GFX &surface) {
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

void DisplayController::drawStatus(Adafruit_GFX &surface) {
  surface.fillRect(0, kFooterTop, touch_ui::kDisplayWidth,
                   touch_ui::kDisplayHeight - kFooterTop, kBackground);
  drawTextClipped(surface, status_, 5, kFooterTop + 2, 1, statusColor_,
                  touch_ui::kDisplayWidth - 10);
}

void DisplayController::drawTextClipped(Adafruit_GFX &surface,
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

void DisplayController::drawReset(Adafruit_GFX &surface, int16_t x, int16_t y,
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

void DisplayController::drawPeriod(Adafruit_GFX &surface, const char *label,
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

void DisplayController::showDisplayTab(uint32_t selectedTimeoutSec) {
  const bool changed = activeTab_ != touch_ui::Tab::kDisplay ||
                       displayedTimeoutSec_ != selectedTimeoutSec;
  activeTab_ = touch_ui::Tab::kDisplay;
  displayedTimeoutSec_ = selectedTimeoutSec;
  if (!hasPresentedScreen_ || changed) {
    presentScreen();
  }
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
  tft_.setSPISpeed(2000000);
  tft_.startWrite();
  tft_.setAddrWindow(0, 0, 320, 240);
  tft_.writeCommand(ILI9341_RAMRD);
  tft_.spiRead();
  uint8_t line[960];
  for (int y = 0; y < 240; y++) {
    for (int x = 0; x < 960; x++)
      line[x] = tft_.spiRead();
    Serial.write(line, sizeof(line));
    yield();
  }
  tft_.endWrite();
  tft_.setSPISpeed(40000000);
  tft_.setRotation(screenFlipped_ ? 3 : 1);
  Serial.println();
}
