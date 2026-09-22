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
#include "config_store.h"
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
  void redraw(const usage_model::UsageSnapshot *snapshot, uint32_t timeoutSec,
              uint32_t pollIntervalSec);
  bool consumeBootClick();
  bool consumeBootCalibration();
  void calibrateTouch();
  void capture();
  void renderNoData(const char *message);
  void renderSnapshot(const usage_model::UsageSnapshot &snapshot);
  void showUsageTab();
  void showDisplayTab(uint32_t selectedTimeoutSec, uint32_t pollIntervalSec);
  void showStatus(const char *message, uint16_t color = ILI9341_WHITE);
  // Scrolls the Display-tab content. The offset is clamped to the content.
  void setDisplayScroll(int16_t scroll);
  // Updates the "next poll" countdown shown in the footer. remainingMs is
  // UINT32_MAX while the cadence is unknown. Returns true when the displayed
  // second changed (and the footer was refreshed).
  bool updatePollCountdown(uint32_t remainingMs, uint32_t pollIntervalSec);

  bool pollTouch(TouchEvent &event);
  bool hasTouchPending();
  touch_ui::Tab activeTab() const { return activeTab_; }
  bool touchCalibrated() const { return touchCalibration_.configured; }
  int16_t touchPressureThreshold() const { return touchCalibration_.pressure; }

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
  uint32_t displayedPollSec_ = config_store::kDefaultPollIntervalSec;
  int16_t displayScroll_ = 0;
  uint32_t pollCountdownSec_ = UINT32_MAX;
  bool hasDisplayedSnapshot_ = false;
  bool hasPresentedScreen_ = false;
  esp_timer_handle_t backlightTimer_ = nullptr;
  mutable portMUX_TYPE backlightMux_ = portMUX_INITIALIZER_UNLOCKED;
  backlight_timer::Model backlight_;
  volatile bool touchWakePending_ = false;
  volatile bool touchReadPending_ = false;
  bool backlightStateChanged_ = false;
  bool backlightTimerReady_ = false;
  touch_ui::Tab activeTab_ = touch_ui::Tab::kUsage;
  touch_ui::ReleaseLatch touchLatch_;
  touch_ui::ActionKind lastDragKind_ = touch_ui::ActionKind::kNone;
  uint32_t lastDragValue_ = 0;
  // Whole-content drag scrolling: the tap that starts a contact fixes the
  // gesture mode (slider adjust, scrollbar jump, or relative scroll).
  bool dragActive_ = false;
  touch_ui::ActionKind dragMode_ = touch_ui::ActionKind::kNone;
  bool dragStartedInHeader_ = false;
  int16_t dragStartX_ = 0;
  int16_t dragStartY_ = 0;
  int16_t dragStartScroll_ = 0;
  boot_button::Model bootButton_;
  uint32_t pendingBootClicks_ = 0;
  uint32_t pendingBootCalibrations_ = 0;
  bool screenFlipped_ = false;
  struct TouchCalibration {
    touch_ui::CalibratedMap map;
    int16_t pressure = 120;
    bool configured = false;
  } touchCalibration_;
};
