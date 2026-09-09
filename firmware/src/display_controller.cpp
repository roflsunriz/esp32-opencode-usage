#include "display_controller.h"

#include "board_pins.h"

#include <cstdio>
#include <cstring>

namespace {
constexpr uint16_t kBackground = 0x1082;
constexpr uint16_t kPanel = 0x18C3;
constexpr uint16_t kBarBackground = 0x4208;
constexpr int16_t kFooterTop = 228;
constexpr int16_t kRowHeight = 68;
constexpr int16_t kFirstRowTop = 23;

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
    : spi_(VSPI),
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
}

void DisplayController::drawHeader() {
  tft_.fillRect(0, 0, tft_.width(), 21, ILI9341_BLACK);
  tft_.setTextColor(ILI9341_CYAN, ILI9341_BLACK);
  tft_.setTextSize(2);
  tft_.setCursor(4, 3);
  tft_.print("OpenCode Go");
  tft_.setTextColor(ILI9341_LIGHTGREY, ILI9341_BLACK);
  tft_.setTextSize(1);
  tft_.setCursor(258, 7);
  tft_.print("ESP32");
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
  tft_.fillScreen(kBackground);
  drawHeader();
  drawPeriod("5H", snapshot.rolling, kFirstRowTop);
  drawPeriod("WEEK", snapshot.weekly, kFirstRowTop + kRowHeight);
  drawPeriod("MONTH", snapshot.monthly, kFirstRowTop + kRowHeight * 2);
  showStatus("Usage updated", ILI9341_GREEN);
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
