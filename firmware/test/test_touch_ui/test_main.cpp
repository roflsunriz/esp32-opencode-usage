#include <unity.h>

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

void test_filters_five_raw_samples_with_median() {
  const uint16_t samples[] = {100, 101, 4000, 99, 102};
  TEST_ASSERT_EQUAL_UINT16(101, touch_ui::median5(samples));
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
  RUN_TEST(test_filters_five_raw_samples_with_median);
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
