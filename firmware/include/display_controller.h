#pragma once

#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>

#include <cstddef>

#include "usage_model.h"

class DisplayController {
public:
  DisplayController();

  void begin();
  void capture();
  void renderNoData(const char *message);
  void renderSnapshot(const usage_model::UsageSnapshot &snapshot);
  void showStatus(const char *message, uint16_t color = ILI9341_WHITE);

private:
  void drawHeader();
  void drawPeriod(const char *label, const usage_model::PeriodUsage &period,
                  int16_t top);
  void drawReset(int16_t x, int16_t y, int64_t resetInSec);
  void drawTextClipped(const char *message, int16_t x, int16_t y,
                       uint8_t textSize, uint16_t color, uint16_t maxWidth);

  SPIClass spi_;
  Adafruit_ILI9341 tft_;
  char status_[72] = "Waiting for usage";
  uint16_t statusColor_ = ILI9341_WHITE;
};
