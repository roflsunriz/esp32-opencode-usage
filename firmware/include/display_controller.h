#pragma once

#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

#include <cstddef>
#include <cstdint>

#include "backlight_timer.h"
#include "boot_button.h"
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
  void presentScreen();
  void presentStatusBand();
  void presentBand(uint16_t top);
  void drawScreen(Adafruit_GFX &surface);
  void drawHeader(Adafruit_GFX &surface);
  void drawDisplaySettings(Adafruit_GFX &surface,
                           uint32_t selectedTimeoutSec);
  void drawUsage(Adafruit_GFX &surface);
  void drawStatus(Adafruit_GFX &surface);
  void drawPeriod(Adafruit_GFX &surface, const char *label,
                  const usage_model::PeriodUsage &period, int16_t top);
  void drawReset(Adafruit_GFX &surface, int16_t x, int16_t y,
                 int64_t resetInSec);
  void drawTextClipped(Adafruit_GFX &surface, const char *message, int16_t x,
                       int16_t y, uint8_t textSize, uint16_t color,
                       uint16_t maxWidth);
  bool updateStatus(const char *message, uint16_t color);
  bool updateSnapshot(const usage_model::UsageSnapshot &snapshot);

  SPIClass spi_;
  SPIClass touchSpi_;
  Adafruit_ILI9341 tft_;
  char status_[72] = "Waiting for usage";
  uint16_t statusColor_ = ILI9341_WHITE;
  // One RGB565 scanline band. A full 320x240 framebuffer would consume about
  // 150 KiB of DRAM needed by TLS, while this fixed 10 KiB buffer lets every
  // transfer replace only complete image bands.
  static constexpr uint16_t kFrameBandHeight = 16;
  uint16_t frameBand_[touch_ui::kDisplayWidth * kFrameBandHeight] = {};
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
  bool screenFlipped_ = false;
};
