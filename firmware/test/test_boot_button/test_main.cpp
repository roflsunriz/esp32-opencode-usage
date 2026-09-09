#include <unity.h>

#include "boot_button.h"
#include "touch_ui.h"

void setUp() {}
void tearDown() {}

void test_click_requires_stable_press_and_release_without_hold_repeat() {
  boot_button::Model button;
  button.start(false, 0);
  TEST_ASSERT_FALSE(button.update(true, 10));
  TEST_ASSERT_FALSE(button.update(true, 39));
  TEST_ASSERT_FALSE(button.update(true, 40));
  TEST_ASSERT_FALSE(button.update(true, 10000));
  TEST_ASSERT_FALSE(button.update(false, 10001));
  TEST_ASSERT_FALSE(button.update(false, 10030));
  TEST_ASSERT_TRUE(button.update(false, 10031));
  TEST_ASSERT_FALSE(button.update(false, 20000));
  TEST_ASSERT_FALSE(button.update(true, 20001));
  TEST_ASSERT_FALSE(button.update(true, 20031));
  TEST_ASSERT_FALSE(button.update(false, 20040));
  TEST_ASSERT_TRUE(button.update(false, 20070));
}

void test_bounce_does_not_click_or_repeat() {
  boot_button::Model button;
  button.start(false, 0);
  TEST_ASSERT_FALSE(button.update(true, 10));
  TEST_ASSERT_FALSE(button.update(false, 20));
  TEST_ASSERT_FALSE(button.update(false, 100));
  TEST_ASSERT_FALSE(button.update(true, 110));
  TEST_ASSERT_FALSE(button.update(true, 140));
  TEST_ASSERT_FALSE(button.update(false, 150));
  TEST_ASSERT_FALSE(button.update(true, 160));
  TEST_ASSERT_FALSE(button.update(false, 170));
  TEST_ASSERT_FALSE(button.update(false, 199));
  TEST_ASSERT_TRUE(button.update(false, 200));
  TEST_ASSERT_FALSE(button.update(false, 300));
}

void test_startup_held_button_is_ignored_and_clock_wrap_is_safe() {
  boot_button::Model button;
  button.start(true, 0);
  TEST_ASSERT_FALSE(button.update(true, 50));
  TEST_ASSERT_FALSE(button.update(false, 100));
  TEST_ASSERT_FALSE(button.update(false, 130));
  button.start(false, UINT32_MAX - 50);
  TEST_ASSERT_FALSE(button.update(true, UINT32_MAX - 40));
  TEST_ASSERT_FALSE(button.update(true, UINT32_MAX - 10));
  TEST_ASSERT_FALSE(button.update(false, UINT32_MAX - 5));
  TEST_ASSERT_FALSE(button.update(false, 23));
  TEST_ASSERT_TRUE(button.update(false, 24));
}

void test_flipped_touch_maps_all_buttons_and_tabs() {
  const uint16_t xPositions[] = {55, 160, 265};
  const uint16_t yPositions[] = {70, 126, 182};
  for (uint16_t row = 0; row < 3; ++row) {
    for (uint16_t column = 0; column < 3; ++column) {
      const uint16_t rawX = touch_ui::kRawXMin +
                            (239 - yPositions[row]) *
                                (touch_ui::kRawXMax - touch_ui::kRawXMin) / 239;
      const uint16_t rawY = touch_ui::kRawYMin +
                            (319 - xPositions[column]) *
                                (touch_ui::kRawYMax - touch_ui::kRawYMin) / 319;
      const auto point = touch_ui::mapPoint(rawX, rawY, true);
      const auto action =
          touch_ui::hitTest(touch_ui::Tab::kDisplay, point.x, point.y);
      TEST_ASSERT_EQUAL_UINT32(
          backlight_timer::kTimeoutOptionsSec[row * 3 + column],
          action.timeoutSec);
    }
  }
  const auto topLeft = touch_ui::mapPoint(4095, 4095, true);
  TEST_ASSERT_EQUAL_UINT16(0, topLeft.x);
  TEST_ASSERT_EQUAL_UINT16(0, topLeft.y);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(touch_ui::ActionKind::kUsageTab),
      static_cast<int>(
          touch_ui::hitTest(touch_ui::Tab::kDisplay, topLeft.x, topLeft.y)
              .kind));
  const auto topRight = touch_ui::mapPoint(4095, 0, true);
  TEST_ASSERT_EQUAL_UINT16(319, topRight.x);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(touch_ui::ActionKind::kDisplayTab),
      static_cast<int>(
          touch_ui::hitTest(touch_ui::Tab::kUsage, topRight.x, topRight.y)
              .kind));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_click_requires_stable_press_and_release_without_hold_repeat);
  RUN_TEST(test_bounce_does_not_click_or_repeat);
  RUN_TEST(test_startup_held_button_is_ignored_and_clock_wrap_is_safe);
  RUN_TEST(test_flipped_touch_maps_all_buttons_and_tabs);
  return UNITY_END();
}
