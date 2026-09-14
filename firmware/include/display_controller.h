#pragma once

#include <TFT_eSPI.h>
#include <sensitive-xpt2046.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

#include <cstddef>
#include <cstdint>

#include "backlight_timer.h"
#include "board_pins.h"
#include "boot_button.h"
#include "display-diff.h"
#include "touch_ui.h"
#include "usage_model.h"

enum class TouchEventKind : uint8_t { kWakeOnly, kTap, kUnreadable };

struct TouchEvent {
  TouchEvent()
      : kind(TouchEventKind::kUnreadable), action(), point(),
        hasCoordinates(false) {}

  TouchEventKind kind;
  touch_ui::Action action;
  touch_ui::Point point;
  bool hasCoordinates;
};

class DisplayController {
public:
  DisplayController();

  void begin(bool flipped = false);
  void setScreenFlipped(bool flipped);
  void redraw(const usage_model::UsageSnapshot *snapshot, uint32_t timeoutSec);
  bool consumeBootClick();
  bool consumeBootCalibration();
  void calibrateTouch();
  void capture();
  void renderNoData(const char *message);
  void renderSnapshot(const usage_model::UsageSnapshot &snapshot);
  void showUsageTab();
  void showDisplayTab(uint32_t selectedTimeoutSec);
  void showStatus(const char *message, uint16_t color = ILI9341_WHITE);

  bool pollTouch(TouchEvent &event);
  bool hasTouchPending();
  touch_ui::Tab activeTab() const { return activeTab_; }

  bool setBacklightTimeoutSec(uint32_t timeoutSec);
  void wakeBacklight();
  bool consumeBacklightStateChanged();
  bool backlightOn();
  uint32_t backlightTimeoutSec();
  uint32_t backlightRemainingMs();
  int backlightGpioLevel() const;

private:
  static void backlightTimerCallback(void *context);
  static void IRAM_ATTR touchInterrupt(void *context);
  void handleBacklightTimer();
  void IRAM_ATTR handleTouchInterrupt();
  bool wakeForTouchLocked(uint32_t currentMs);
  void setBacklightPinLocked(bool on);
  uint32_t nowMs() const;
  void armTouchInterrupt();
  bool readTouchPoint(touch_ui::Point &point);
  bool captureCalibrationPoint(int16_t &rawX, int16_t &rawY,
                               int16_t &pressure);
  void loadCalibration();
  bool saveCalibration();
  void presentScreen();
  void presentStatusBand();
  void drawScreen(TFT_eSPI &surface);
  void drawHeader(TFT_eSPI &surface);
  void drawDisplaySettings(TFT_eSPI &surface,
                           uint32_t selectedTimeoutSec);
  void drawUsage(TFT_eSPI &surface);
  void drawStatus(TFT_eSPI &surface);
  void drawPeriod(TFT_eSPI &surface, const char *label,
                  const usage_model::PeriodUsage &period, int16_t top);
  void drawReset(TFT_eSPI &surface, int16_t x, int16_t y,
                 int64_t resetInSec);
  void drawTextClipped(TFT_eSPI &surface, const char *message, int16_t x,
                       int16_t y, uint8_t textSize, uint16_t color,
                       uint16_t maxWidth);
  bool updateStatus(const char *message, uint16_t color);
  bool updateSnapshot(const usage_model::UsageSnapshot &snapshot);

  SPIClass touchSpi_;
  TFT_eSPI tft_;
  TFT_eSprite canvas_;
  SensitiveXpt2046 touch_;
  char status_[72] = "Waiting for usage";
  uint16_t statusColor_ = ILI9341_WHITE;
  bool canvasReady_ = false;
  display_diff::Bands bandDiff_;
  usage_model::UsageSnapshot displayedSnapshot_;
  uint32_t displayedTimeoutSec_ = backlight_timer::kDefaultTimeoutSec;
  bool hasDisplayedSnapshot_ = false;
  bool hasPresentedScreen_ = false;
  esp_timer_handle_t backlightTimer_ = nullptr;
  mutable portMUX_TYPE backlightMux_ = portMUX_INITIALIZER_UNLOCKED;
  backlight_timer::Model backlight_;
  volatile bool touchWakePending_ = false;
  volatile bool touchReadPending_ = false;
  bool touchWakeOnly_ = false;
  bool backlightStateChanged_ = false;
  bool backlightTimerReady_ = false;
  touch_ui::Tab activeTab_ = touch_ui::Tab::kUsage;
  touch_ui::ReleaseLatch touchLatch_;
  boot_button::Model bootButton_;
  uint32_t pendingBootClicks_ = 0;
  uint32_t pendingBootCalibrations_ = 0;
  bool screenFlipped_ = false;
  struct TouchCalibration {
    int16_t left = touch_ui::kRawYMin, right = touch_ui::kRawYMax;
    int16_t top = touch_ui::kRawXMin, bottom = touch_ui::kRawXMax;
    int16_t pressure = 120;
    bool configured = false;
  } touchCalibration_;
};
