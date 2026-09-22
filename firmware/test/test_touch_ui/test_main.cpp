#include <array>
#include <unity.h>

#include "display-diff.h"
#include "touch_ui.h"

void test_maps_measured_swapped_rotation_one_calibration() {
  const touch_ui::Point topLeft =
      touch_ui::mapPoint(touch_ui::kRawXMin, touch_ui::kRawYMin);
  TEST_ASSERT_EQUAL_UINT16(0, topLeft.x);
  TEST_ASSERT_EQUAL_UINT16(0, topLeft.y);

  const touch_ui::Point bottomRight =
      touch_ui::mapPoint(touch_ui::kRawXMax, touch_ui::kRawYMax);
  TEST_ASSERT_EQUAL_UINT16(319, bottomRight.x);
  TEST_ASSERT_EQUAL_UINT16(239, bottomRight.y);

  const touch_ui::Point displayTap = touch_ui::mapPoint(600, 2600);
  TEST_ASSERT_EQUAL_UINT16(205, displayTap.x);
  TEST_ASSERT_EQUAL_UINT16(21, displayTap.y);
  TEST_ASSERT_TRUE(touch_ui::isPlausibleRaw(2000, 2000));
  TEST_ASSERT_FALSE(touch_ui::isPlausibleRaw(0, 2000));
}

void test_migrated_legacy_map_matches_previous_behavior() {
  const touch_ui::CalibratedMap map =
      touch_ui::calibratedFromLegacy(340, 3860, 280, 3860);
  TEST_ASSERT_FALSE(map.useRawXForX);
  TEST_ASSERT_FALSE(map.useRawYForY);
  TEST_ASSERT_TRUE(touch_ui::calibratedSpansValid(map));
  const auto leftTop = touch_ui::mapPointWithMap(280, 340, false, map);
  TEST_ASSERT_EQUAL_UINT16(24, leftTop.x);
  TEST_ASSERT_EQUAL_UINT16(24, leftTop.y);
  const auto rightBottom = touch_ui::mapPointWithMap(3860, 3860, false, map);
  TEST_ASSERT_EQUAL_UINT16(295, rightBottom.x);
  TEST_ASSERT_EQUAL_UINT16(215, rightBottom.y);
  const auto inverted = touch_ui::mapPointWithMap(280, 340, true, map);
  TEST_ASSERT_EQUAL_UINT16(295, inverted.x);
  TEST_ASSERT_EQUAL_UINT16(215, inverted.y);
  const touch_ui::CalibratedMap reversed =
      touch_ui::calibratedFromLegacy(3860, 340, 3860, 280);
  const auto mirrored = touch_ui::mapPointWithMap(3860, 3860, false, reversed);
  TEST_ASSERT_EQUAL_UINT16(24, mirrored.x);
  TEST_ASSERT_EQUAL_UINT16(24, mirrored.y);
  TEST_ASSERT_EQUAL_INT16(12, touch_ui::pressureThresholdFor(10));
  TEST_ASSERT_EQUAL_INT16(15, touch_ui::pressureThresholdFor(30));
  TEST_ASSERT_EQUAL_INT16(120, touch_ui::pressureThresholdFor(600));
}

void test_builds_straight_panel_map_from_measured_rows_and_columns() {
  // COM6 board (2026-09-16 diagnostics): rawX follows screen x and rawY
  // follows screen y, so the row pair varies rawX and the column pair rawY.
  const touch_ui::CalibratedMap map = touch_ui::buildCalibratedMap(
      {400, 600}, {3400, 700}, {450, 3500});
  TEST_ASSERT_TRUE(map.useRawXForX);
  TEST_ASSERT_TRUE(map.useRawYForY);
  TEST_ASSERT_EQUAL_INT16(400, map.xStart);
  TEST_ASSERT_EQUAL_INT16(3400, map.xEnd);
  TEST_ASSERT_EQUAL_INT16(600, map.yStart);
  TEST_ASSERT_EQUAL_INT16(3500, map.yEnd);
  TEST_ASSERT_TRUE(touch_ui::calibratedSpansValid(map));
  const auto center = touch_ui::mapPointWithMap(1900, 2050, false, map);
  TEST_ASSERT_EQUAL_UINT16(159, center.x);
  TEST_ASSERT_EQUAL_UINT16(119, center.y);
  const auto flipped = touch_ui::mapPointWithMap(1900, 2050, true, map);
  TEST_ASSERT_EQUAL_UINT16(160, flipped.x);
  TEST_ASSERT_EQUAL_UINT16(120, flipped.y);
}

void test_builds_swapped_panel_map_from_measured_rows_and_columns() {
  // 2026-09-09 board: Display-tab taps read rawX≈500..800 with
  // rawY≈2500..2900, so the row pair varies rawY and the column pair rawX.
  const touch_ui::CalibratedMap map = touch_ui::buildCalibratedMap(
      {600, 400}, {700, 3400}, {3500, 450});
  TEST_ASSERT_FALSE(map.useRawXForX);
  TEST_ASSERT_FALSE(map.useRawYForY);
  TEST_ASSERT_EQUAL_INT16(400, map.xStart);
  TEST_ASSERT_EQUAL_INT16(3400, map.xEnd);
  TEST_ASSERT_EQUAL_INT16(600, map.yStart);
  TEST_ASSERT_EQUAL_INT16(3500, map.yEnd);
  TEST_ASSERT_TRUE(touch_ui::calibratedSpansValid(map));
  const auto center = touch_ui::mapPointWithMap(2050, 1900, false, map);
  TEST_ASSERT_EQUAL_UINT16(159, center.x);
  TEST_ASSERT_EQUAL_UINT16(119, center.y);
}

void test_rejects_calibration_without_sufficient_spans() {
  const touch_ui::CalibratedMap map =
      touch_ui::buildCalibratedMap({100, 100}, {150, 120}, {110, 160});
  TEST_ASSERT_FALSE(touch_ui::calibratedSpansValid(map));
}

void test_only_changed_display_bands_are_transferred() {
  std::array<uint8_t, display_diff::kWidth * display_diff::kHeight> frame{};
  display_diff::Bands bands;
  TEST_ASSERT_EQUAL_HEX16(0x7FFF, bands.update(frame.data()));
  TEST_ASSERT_EQUAL_HEX16(0, bands.update(frame.data()));
  frame[1 * display_diff::kWidth + 4] = 2;
  TEST_ASSERT_EQUAL_HEX16(1, bands.update(frame.data()));
  frame[228 * display_diff::kWidth + 5] = 3;
  TEST_ASSERT_EQUAL_HEX16(1U << 14, bands.update(frame.data()));
  bands.invalidate();
  TEST_ASSERT_EQUAL_HEX16(0x7FFF, bands.update(frame.data()));
  size_t runs = 0;
  TEST_ASSERT_TRUE(display_diff::eachRun(
      static_cast<uint16_t>((1U << 2) | (1U << 3) | (1U << 6)),
      [&](size_t top, size_t height) {
        if (runs == 0) { TEST_ASSERT_EQUAL_UINT32(32, top); TEST_ASSERT_EQUAL_UINT32(32, height); }
        if (runs == 1) { TEST_ASSERT_EQUAL_UINT32(96, top); TEST_ASSERT_EQUAL_UINT32(16, height); }
        ++runs; return true;
      }));
  TEST_ASSERT_EQUAL_UINT32(2, runs);
}

void test_sprite_clear_covers_full_320_pixel_width() {
  struct Surface {
    int width = 0, height = 0;
    void fillRect(int, int, int w, int h, uint32_t) {
      width = w; height = h;
    }
  } surface;
  display_diff::clearFrame(surface, 0x1082);
  TEST_ASSERT_EQUAL_INT(320, surface.width);
  TEST_ASSERT_EQUAL_INT(240, surface.height);
}

void test_slider_value_round_trips_with_track_ends() {
  TEST_ASSERT_EQUAL_UINT32(
      0, touch_ui::sliderValueFromX(touch_ui::kSliderTrackX0, 0, 59, 1));
  TEST_ASSERT_EQUAL_UINT32(
      59, touch_ui::sliderValueFromX(touch_ui::kSliderTrackX1, 0, 59, 1));
  TEST_ASSERT_EQUAL_UINT32(
      60, touch_ui::sliderValueFromX(touch_ui::kSliderTrackX0, 60, 600, 60));
  TEST_ASSERT_EQUAL_UINT32(
      600, touch_ui::sliderValueFromX(touch_ui::kSliderTrackX1, 60, 600, 60));
  TEST_ASSERT_EQUAL_INT16(
      touch_ui::kSliderTrackX0, touch_ui::sliderXFromValue(0, 0, 59));
  TEST_ASSERT_EQUAL_INT16(
      touch_ui::kSliderTrackX1, touch_ui::sliderXFromValue(59, 0, 59));
  // Out-of-track taps clamp instead of producing out-of-range values.
  TEST_ASSERT_EQUAL_UINT32(
      0, touch_ui::sliderValueFromX(-100, 0, 59, 1));
  TEST_ASSERT_EQUAL_UINT32(
      600, touch_ui::sliderValueFromX(999, 60, 600, 60));
  // Poll slider quantizes to 60-second steps.
  const uint32_t middle = touch_ui::sliderValueFromX(
      (touch_ui::kSliderTrackX0 + touch_ui::kSliderTrackX1) / 2, 60, 600,
      60);
  TEST_ASSERT_EQUAL_UINT32(0, middle % 60);
}

void test_slider_rows_hit_test_with_content_coordinates() {
  const int16_t minuteX =
      touch_ui::sliderXFromValue(30, 0, 59);
  touch_ui::Action minutes =
      touch_ui::hitTestDisplaySliders(minuteX, touch_ui::kSliderMinutesY);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(touch_ui::ActionKind::kSleepMinutes),
      static_cast<int>(minutes.kind));
  TEST_ASSERT_EQUAL_UINT32(30, minutes.timeoutSec);

  const int16_t hourX = touch_ui::sliderXFromValue(2, 0, 24);
  touch_ui::Action hours =
      touch_ui::hitTestDisplaySliders(hourX, touch_ui::kSliderHoursY);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(touch_ui::ActionKind::kSleepHours),
      static_cast<int>(hours.kind));
  TEST_ASSERT_EQUAL_UINT32(2, hours.timeoutSec);

  const int16_t pollX = touch_ui::sliderXFromValue(300, 60, 600);
  touch_ui::Action poll =
      touch_ui::hitTestDisplaySliders(pollX, touch_ui::kSliderPollY);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(touch_ui::ActionKind::kPollInterval),
      static_cast<int>(poll.kind));
  TEST_ASSERT_EQUAL_UINT32(300, poll.timeoutSec);

  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(touch_ui::ActionKind::kNone),
      static_cast<int>(
          touch_ui::hitTestDisplaySliders(minuteX, 10).kind));
  TEST_ASSERT_EQUAL_INT(0, touch_ui::clampScroll(-5));
  TEST_ASSERT_EQUAL_INT(touch_ui::kDisplayScrollMax,
                        touch_ui::clampScroll(9999));
}

void test_latches_each_contact_until_penirq_is_high_for_twenty_ms() {
  touch_ui::ReleaseLatch latch;
  TEST_ASSERT_TRUE(latch.accept());
  TEST_ASSERT_TRUE(latch.isLatched());
  TEST_ASSERT_FALSE(latch.accept());
  latch.update(true, 100);
  latch.update(false, 101);
  latch.update(false, 120);
  TEST_ASSERT_TRUE(latch.isLatched());
  latch.update(false, 121);
  TEST_ASSERT_FALSE(latch.isLatched());
  TEST_ASSERT_TRUE(latch.accept());
}

void test_switches_tabs_only_in_the_header() {
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(touch_ui::ActionKind::kUsageTab),
      static_cast<int>(
          touch_ui::hitTest(touch_ui::Tab::kDisplay, 10, 10).kind));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(touch_ui::ActionKind::kDisplayTab),
      static_cast<int>(touch_ui::hitTest(touch_ui::Tab::kUsage, 310, 10).kind));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(touch_ui::ActionKind::kNone),
      static_cast<int>(touch_ui::hitTest(touch_ui::Tab::kUsage, 10, 60).kind));
}

void test_display_tab_routes_sliders_and_scrollbar() {
  // Slider rows resolve through the tab-level hit test without scrolling.
  const int16_t minuteX = touch_ui::sliderXFromValue(30, 0, 59);
  const touch_ui::Action routed = touch_ui::hitTest(
      touch_ui::Tab::kDisplay, static_cast<uint16_t>(minuteX),
      static_cast<uint16_t>(touch_ui::kSliderMinutesY));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(touch_ui::ActionKind::kSleepMinutes),
      static_cast<int>(routed.kind));
  TEST_ASSERT_EQUAL_UINT32(30, routed.timeoutSec);
  // A scrolled tab offsets content coordinates: the same screen tap now
  // lands 20 content pixels lower.
  const touch_ui::Action scrolled = touch_ui::hitTest(
      touch_ui::Tab::kDisplay, static_cast<uint16_t>(minuteX),
      static_cast<uint16_t>(touch_ui::kSliderMinutesY - 20), 20);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(touch_ui::ActionKind::kSleepMinutes),
      static_cast<int>(scrolled.kind));
  // Scrollbar tap below the thumb pages down from the top.
  const touch_ui::Action page = touch_ui::hitTest(
      touch_ui::Tab::kDisplay, 312, 210);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(touch_ui::ActionKind::kDisplayScroll),
      static_cast<int>(page.kind));
  TEST_ASSERT_EQUAL_UINT32(touch_ui::kScrollPage, page.timeoutSec);
  // Usage tab has no Display content actions.
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(touch_ui::ActionKind::kNone),
      static_cast<int>(
          touch_ui::hitTest(touch_ui::Tab::kUsage, 20, 100).kind));
}

void setUp() {}
void tearDown() {}

int runTests() {
  UNITY_BEGIN();
  RUN_TEST(test_maps_measured_swapped_rotation_one_calibration);
  RUN_TEST(test_migrated_legacy_map_matches_previous_behavior);
  RUN_TEST(test_builds_straight_panel_map_from_measured_rows_and_columns);
  RUN_TEST(test_builds_swapped_panel_map_from_measured_rows_and_columns);
  RUN_TEST(test_rejects_calibration_without_sufficient_spans);
  RUN_TEST(test_only_changed_display_bands_are_transferred);
  RUN_TEST(test_sprite_clear_covers_full_320_pixel_width);
  RUN_TEST(test_latches_each_contact_until_penirq_is_high_for_twenty_ms);
  RUN_TEST(test_slider_value_round_trips_with_track_ends);
  RUN_TEST(test_slider_rows_hit_test_with_content_coordinates);
  RUN_TEST(test_switches_tabs_only_in_the_header);
  RUN_TEST(test_display_tab_routes_sliders_and_scrollbar);
  return UNITY_END();
}

#ifdef ARDUINO
void setup() { runTests(); }
void loop() {}
#else
int main() { return runTests(); }
#endif
