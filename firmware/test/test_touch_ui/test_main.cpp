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

void test_maps_all_nine_display_buttons_to_the_supported_timeouts() {
  for (uint16_t row = 0; row < touch_ui::kGridRows; ++row) {
    for (uint16_t column = 0; column < touch_ui::kGridColumns; ++column) {
      const uint16_t x =
          touch_ui::kGridLeft +
          column * (touch_ui::kButtonWidth + touch_ui::kGridGapX) +
          touch_ui::kButtonWidth / 2;
      const uint16_t y = touch_ui::kGridTop +
                         row * (touch_ui::kButtonHeight + touch_ui::kGridGapY) +
                         touch_ui::kButtonHeight / 2;
      const touch_ui::Action action =
          touch_ui::hitTest(touch_ui::Tab::kDisplay, x, y);
      const size_t index = row * touch_ui::kGridColumns + column;
      TEST_ASSERT_EQUAL_INT(static_cast<int>(touch_ui::ActionKind::kTimeout),
                            static_cast<int>(action.kind));
      TEST_ASSERT_EQUAL_UINT32(backlight_timer::kTimeoutOptionsSec[index],
                               action.timeoutSec);
    }
  }
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
  RUN_TEST(test_switches_tabs_only_in_the_header);
  RUN_TEST(test_maps_all_nine_display_buttons_to_the_supported_timeouts);
  return UNITY_END();
}

#ifdef ARDUINO
void setup() { runTests(); }
void loop() {}
#else
int main() { return runTests(); }
#endif
