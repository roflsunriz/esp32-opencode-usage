#pragma once

#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

#include <cstddef>
#include <cstdint>

#include "backlight_timer.h"
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

  void begin();
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
  uint16_t readTouchCoordinate(uint8_t command);
  void drawHeader();
  void drawDisplaySettings(uint32_t selectedTimeoutSec);
  void drawPeriod(const char *label, const usage_model::PeriodUsage &period,
                  int16_t top);
  void drawReset(int16_t x, int16_t y, int64_t resetInSec);
  void drawTextClipped(const char *message, int16_t x, int16_t y,
                       uint8_t textSize, uint16_t color, uint16_t maxWidth);

  SPIClass spi_;
  SPIClass touchSpi_;
  Adafruit_ILI9341 tft_;
  char status_[72] = "Waiting for usage";
  uint16_t statusColor_ = ILI9341_WHITE;
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
};
