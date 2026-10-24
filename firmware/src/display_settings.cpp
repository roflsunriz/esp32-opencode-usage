#include "display_controller.h"

#include <cstdio>

namespace {
constexpr uint16_t kBackground = 0x1082;
constexpr uint16_t kBarBackground = 0x4208;
constexpr uint16_t kActiveTab = ILI9341_CYAN;
constexpr uint16_t kButton = 0x2965;
constexpr int16_t kFooterTop = touch_ui::kFooterTop;
}  // namespace

void DisplayController::drawDisplaySettings(TFT_eSPI &surface,
                                             uint32_t selectedTimeoutSec) {
  const uint32_t minutes = backlight_timer::sleepMinutesPart(selectedTimeoutSec);
  const uint32_t hours = backlight_timer::sleepHoursPart(selectedTimeoutSec);
  const int16_t scroll = touch_ui::clampScroll(displayScroll_);
  auto contentY = [scroll](int16_t y) {
    return static_cast<int16_t>(y - scroll);
  };
  auto drawSlider = [&](int16_t centerY, uint32_t value, uint32_t minV,
                        uint32_t maxV) {
    const int16_t y = contentY(centerY);
    if (y < touch_ui::kTabHeight + 8 || y > kFooterTop - 9) return;
    surface.drawRect(touch_ui::kSliderTrackX0, y - 2,
                     touch_ui::kSliderTrackX1 - touch_ui::kSliderTrackX0, 5,
                     ILI9341_WHITE);
    const int16_t thumbX = touch_ui::sliderXFromValue(value, minV, maxV);
    surface.fillRect(touch_ui::kSliderTrackX0, y - 2,
                     thumbX - touch_ui::kSliderTrackX0, 5, kActiveTab);
    surface.fillRect(thumbX - 6, y - 8, 12, 17, ILI9341_WHITE);
    surface.fillRect(thumbX - 4, y - 6, 8, 13, kButton);
  };
  auto drawWheel = [&](int16_t x0, int16_t x1, uint32_t value,
                       uint32_t maximum, const char *label) {
    const int16_t labelY = contentY(62);
    if (labelY >= touch_ui::kTabHeight && labelY < kFooterTop - 10) {
      surface.setTextColor(ILI9341_LIGHTGREY, kBackground);
      surface.setTextSize(1);
      const int16_t width = surface.textWidth(label);
      surface.setCursor(x0 + ((x1 - x0) - width) / 2, labelY);
      surface.print(label);
    }
    const int16_t center = contentY(touch_ui::kWheelCenterY);
    if (center >= touch_ui::kTabHeight + 12 && center < kFooterTop - 12) {
      surface.fillRoundRect(x0, center - touch_ui::kWheelRowHeight / 2,
                            x1 - x0, touch_ui::kWheelRowHeight, 3,
                            kBarBackground);
    }
    for (int16_t offset = -2; offset <= 2; ++offset) {
      const int32_t candidate = static_cast<int32_t>(value) + offset;
      if (candidate < 0 || candidate > static_cast<int32_t>(maximum)) continue;
      const int16_t y = center + offset * touch_ui::kWheelRowHeight - 8;
      if (y < touch_ui::kTabHeight || y >= kFooterTop - 10) continue;
      char shown[12] = {};
      snprintf(shown, sizeof(shown), "%02u", static_cast<unsigned>(candidate));
      surface.setTextColor(offset == 0 ? ILI9341_WHITE : ILI9341_LIGHTGREY,
                           offset == 0 ? kBarBackground : kBackground);
      surface.setTextSize(1);
      const int16_t width = surface.textWidth(shown);
      surface.setCursor(x0 + ((x1 - x0) - width) / 2, y);
      surface.print(shown);
    }
  };
  auto drawAction = [&](int16_t contentTop, const char *label, uint16_t fill,
                        bool selected) {
    const int16_t y = contentY(contentTop);
    if (y < touch_ui::kTabHeight ||
        y + touch_ui::kActionButtonHeight > kFooterTop) return;
    const uint16_t color = selected ? kActiveTab : fill;
    surface.fillRoundRect(14, y, 282, touch_ui::kActionButtonHeight, 4, color);
    surface.drawRoundRect(14, y, 282, touch_ui::kActionButtonHeight, 4,
                          selected ? kActiveTab : ILI9341_LIGHTGREY);
    surface.setTextColor(selected ? ILI9341_BLACK : ILI9341_WHITE, color);
    surface.setTextSize(1);
    const int16_t width = surface.textWidth(label);
    surface.setCursor(14 + (282 - width) / 2, y + 8);
    surface.print(label);
  };
  auto drawLine = [&](int16_t y, const char *text, uint16_t color) {
    const int16_t visible = contentY(y);
    if (visible < touch_ui::kTabHeight || visible > kFooterTop - 10) return;
    drawTextClipped(surface, text, 6, visible, 1, color,
                    touch_ui::kDisplayWidth - 30);
  };

  char line[56] = {};
  if (selectedTimeoutSec == 0)
    snprintf(line, sizeof(line), "Off after: always on");
  else
    snprintf(line, sizeof(line), "Off after: %uh %02um", hours, minutes);
  drawLine(46, line, ILI9341_WHITE);
  drawWheel(touch_ui::kHoursWheelX0, touch_ui::kHoursWheelX1, hours,
            backlight_timer::kSleepHoursMax, "HOURS");
  drawWheel(touch_ui::kMinutesWheelX0, touch_ui::kMinutesWheelX1, minutes,
            backlight_timer::kSleepMinutesMax, "MINUTES");
  snprintf(line, sizeof(line), "Update 60-600s: %us", displayedPollSec_);
  drawLine(186, line, ILI9341_LIGHTGREY);
  drawSlider(touch_ui::kSliderPollY, displayedPollSec_,
             backlight_timer::kPollSliderMinSec,
             backlight_timer::kPollSliderMaxSec);
  drawAction(touch_ui::kUsageWakeButtonY, "USAGE CHANGE: 10 MIN", kButton,
             displayedUsageChangeWake_);
  drawAction(touch_ui::kScreenOffButtonY, "TURN SCREEN OFF", ILI9341_RED,
             false);

  const int16_t trackH = touch_ui::kScrollBarY1 - touch_ui::kScrollBarY0;
  surface.drawRect(touch_ui::kScrollBarX0, touch_ui::kScrollBarY0, 12, trackH,
                   kBarBackground);
  const int16_t thumbH = static_cast<int16_t>(
      static_cast<int32_t>(touch_ui::kDisplayVisibleHeight) * trackH /
      touch_ui::kDisplayContentHeight);
  const int16_t travel = trackH - thumbH;
  const int16_t thumbY0 =
      travel <= 0 || touch_ui::kDisplayScrollMax <= 0
          ? touch_ui::kScrollBarY0
          : static_cast<int16_t>(touch_ui::kScrollBarY0 +
                                 static_cast<int32_t>(scroll) * travel /
                                     touch_ui::kDisplayScrollMax);
  surface.fillRect(touch_ui::kScrollBarX0 + 2, thumbY0, 8, thumbH,
                   ILI9341_WHITE);
}
