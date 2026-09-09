#include "display_controller.h"

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
constexpr uint64_t kBacklightTimerPeriodUs = 50000;
// Adafruit's TSC2046 driver uses a conservative 2 MHz SPI default. It is
// below the XPT2046 timing limit and avoids board-to-board read noise.
constexpr uint32_t kTouchSpiFrequencyHz = 2000000;
constexpr uint8_t kEnablePenIrqCommand = 0x90;
constexpr uint8_t kReadXCommand = 0xD0;
constexpr uint8_t kReadYCommand = 0x90;
constexpr size_t kTouchSampleCount = 5;

uint16_t colorForPercent(double percent) {
  if (percent >= 90.0) {
    return ILI9341_RED;
  }
  if (percent >= 75.0) {
    return ILI9341_YELLOW;
  }
  return ILI9341_GREEN;
}

} // namespace

DisplayController::DisplayController()
    : spi_(VSPI), touchSpi_(HSPI),
      tft_(&spi_, board_pins::tft_dc, board_pins::tft_cs, board_pins::tft_rst) {
}

void DisplayController::begin() {
  pinMode(board_pins::tft_backlight, OUTPUT);
  digitalWrite(board_pins::tft_backlight, HIGH);
  spi_.begin(board_pins::tft_sclk, board_pins::tft_miso, board_pins::tft_mosi,
             board_pins::tft_cs);
  tft_.begin(40000000);
  tft_.setRotation(1);
  tft_.setTextWrap(false);
  tft_.fillScreen(kBackground);
  drawHeader();
  showStatus(status_, statusColor_);

  backlight_.start(backlight_timer::kDefaultTimeoutSec, nowMs());
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
  point = touch_ui::mapPoint(medianX, medianY);
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

void DisplayController::drawHeader() {
  const bool usageActive = activeTab_ == touch_ui::Tab::kUsage;
  tft_.fillRect(0, 0, tft_.width(), touch_ui::kTabHeight, ILI9341_BLACK);
  tft_.fillRect(0, 0, touch_ui::kTabWidth, touch_ui::kTabHeight,
                usageActive ? kActiveTab : kInactiveTab);
  tft_.fillRect(touch_ui::kTabWidth, 0, touch_ui::kTabWidth,
                touch_ui::kTabHeight, usageActive ? kInactiveTab : kActiveTab);
  tft_.drawFastHLine(0, touch_ui::kTabHeight - 1, tft_.width(), ILI9341_WHITE);
  tft_.setTextColor(usageActive ? ILI9341_BLACK : ILI9341_WHITE,
                    usageActive ? kActiveTab : kInactiveTab);
  tft_.setTextSize(2);
  tft_.setCursor(32, 12);
  tft_.print("Usage");
  tft_.setTextColor(usageActive ? ILI9341_WHITE : ILI9341_BLACK,
                    usageActive ? kInactiveTab : kActiveTab);
  tft_.setCursor(180, 12);
  tft_.print("Display");
}

void DisplayController::drawDisplaySettings(uint32_t selectedTimeoutSec) {
  tft_.fillScreen(kBackground);
  drawHeader();
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
      tft_.fillRoundRect(left, top, touch_ui::kButtonWidth,
                         touch_ui::kButtonHeight, 5, color);
      tft_.drawRoundRect(left, top, touch_ui::kButtonWidth,
                         touch_ui::kButtonHeight, 5, ILI9341_WHITE);
      const char *label = touch_ui::timeoutLabel(timeoutSec);
      tft_.setTextColor(ILI9341_WHITE, color);
      tft_.setTextSize(2);
      const int16_t textWidth = static_cast<int16_t>(strlen(label) * 12);
      tft_.setCursor(left + (touch_ui::kButtonWidth - textWidth) / 2, top + 17);
      tft_.print(label);
    }
  }
  tft_.setTextColor(ILI9341_LIGHTGREY, kBackground);
  tft_.setTextSize(1);
  tft_.setCursor(6, 215);
  tft_.print("Selected: ");
  tft_.print(touch_ui::timeoutLabel(selectedTimeoutSec));
  showStatus(status_, statusColor_);
}

void DisplayController::drawTextClipped(const char *message, int16_t x,
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
  tft_.setTextColor(color, kBackground);
  tft_.setTextSize(textSize);
  tft_.setCursor(x, y);
  tft_.print(clipped);
}

void DisplayController::drawReset(int16_t x, int16_t y, int64_t resetInSec) {
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
  drawTextClipped(resetText, x, y, 1, ILI9341_LIGHTGREY, 130);
}

void DisplayController::drawPeriod(const char *label,
                                   const usage_model::PeriodUsage &period,
                                   int16_t top) {
  tft_.fillRoundRect(2, top, tft_.width() - 4, kRowHeight - 3, 3, kPanel);
  tft_.setTextColor(ILI9341_WHITE, kPanel);
  tft_.setTextSize(2);
  tft_.setCursor(7, top + 4);
  tft_.print(label);

  if (!period.valid) {
    tft_.setTextSize(1);
    tft_.setTextColor(ILI9341_LIGHTGREY, kPanel);
    tft_.setCursor(64, top + 9);
    tft_.print("waiting for valid data");
    tft_.drawRect(5, top + 27, tft_.width() - 10, 13, kBarBackground);
    drawReset(7, top + 46, -1);
    return;
  }

  char amountText[48] = {};
  snprintf(amountText, sizeof(amountText), "$%.2f / $%.2f", period.used,
           period.limit);
  // Keep the percentage in a fixed right-hand column so long dollar values
  // cannot hide the primary percentage indicator.
  drawTextClipped(amountText, 74, top + 8, 1, ILI9341_WHITE, 176);
  char percentText[16] = {};
  snprintf(percentText, sizeof(percentText), "%5.1f%%", period.percent);
  drawTextClipped(percentText, 254, top + 8, 1, colorForPercent(period.percent),
                  60);

  constexpr uint16_t kBarWidth = 306;
  constexpr uint16_t kBarHeight = 11;
  tft_.drawRect(5, top + 27, kBarWidth + 2, kBarHeight + 2, ILI9341_WHITE);
  tft_.fillRect(7, top + 29, kBarWidth, kBarHeight, kBarBackground);
  const uint16_t fill = usage_model::filledWidth(period.percent, kBarWidth);
  if (fill > 0) {
    tft_.fillRect(7, top + 29, fill, kBarHeight,
                  colorForPercent(period.percent));
  }
  drawReset(7, top + 46, period.resetInSec);
}

void DisplayController::showStatus(const char *message, uint16_t color) {
  if (message != nullptr) {
    snprintf(status_, sizeof(status_), "%s", message);
  }
  statusColor_ = color;
  tft_.fillRect(0, kFooterTop, tft_.width(), tft_.height() - kFooterTop,
                kBackground);
  drawTextClipped(status_, 5, kFooterTop + 2, 1, statusColor_,
                  static_cast<uint16_t>(tft_.width() - 10));
}

void DisplayController::renderNoData(const char *message) {
  if (activeTab_ != touch_ui::Tab::kUsage) {
    return;
  }
  tft_.fillScreen(kBackground);
  drawHeader();
  usage_model::PeriodUsage empty;
  drawPeriod("5H", empty, kFirstRowTop);
  drawPeriod("WEEK", empty, kFirstRowTop + kRowHeight);
  drawPeriod("MONTH", empty, kFirstRowTop + kRowHeight * 2);
  showStatus(message, ILI9341_YELLOW);
}

void DisplayController::renderSnapshot(
    const usage_model::UsageSnapshot &snapshot) {
  if (activeTab_ != touch_ui::Tab::kUsage) {
    return;
  }
  tft_.fillScreen(kBackground);
  drawHeader();
  drawPeriod("5H", snapshot.rolling, kFirstRowTop);
  drawPeriod("WEEK", snapshot.weekly, kFirstRowTop + kRowHeight);
  drawPeriod("MONTH", snapshot.monthly, kFirstRowTop + kRowHeight * 2);
  showStatus("Usage updated", ILI9341_GREEN);
}

void DisplayController::showUsageTab() { activeTab_ = touch_ui::Tab::kUsage; }

void DisplayController::showDisplayTab(uint32_t selectedTimeoutSec) {
  activeTab_ = touch_ui::Tab::kDisplay;
  drawDisplaySettings(selectedTimeoutSec);
}

// Diagnostic readback from the real ILI9341 GRAM. A row buffer avoids
// allocating a second 150 KB framebuffer. RAMRD returns one dummy byte then
// RGB666 bytes.
void DisplayController::capture() {
  Serial.println("{\"type\":\"screenshot\",\"width\":320,\"height\":240,"
                 "\"format\":\"rgb888\",\"bytes\":230400}");
  Serial.flush();
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
  Serial.println();
}
