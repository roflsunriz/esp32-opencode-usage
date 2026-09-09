#include <unity.h>

#include "banded_frame.h"

void setUp() {}
void tearDown() {}

void test_clips_a_shape_to_the_current_band() {
  const banded_frame::Rect area =
      banded_frame::clipToBand(10, 12, 20, 10, 320, 240, 16, 16);
  TEST_ASSERT_TRUE(area.visible);
  TEST_ASSERT_EQUAL_INT16(10, area.x);
  TEST_ASSERT_EQUAL_INT16(16, area.y);
  TEST_ASSERT_EQUAL_INT16(20, area.width);
  TEST_ASSERT_EQUAL_INT16(6, area.height);
}

void test_rejects_shapes_outside_the_band_or_screen() {
  const banded_frame::Rect above =
      banded_frame::clipToBand(0, 0, 20, 10, 320, 240, 16, 16);
  TEST_ASSERT_FALSE(above.visible);
  const banded_frame::Rect below =
      banded_frame::clipToBand(0, 40, 20, 10, 320, 240, 16, 16);
  TEST_ASSERT_FALSE(below.visible);
  const banded_frame::Rect offScreen =
      banded_frame::clipToBand(-30, 16, 20, 10, 320, 240, 16, 16);
  TEST_ASSERT_FALSE(offScreen.visible);
}

void test_full_screen_is_covered_once_by_fifteen_bands() {
  int32_t coveredPixels = 0;
  for (int16_t top = 0; top < 240; top += 16) {
    const banded_frame::Rect area =
        banded_frame::clipToBand(0, 0, 320, 240, 320, 240, top, 16);
    TEST_ASSERT_TRUE(area.visible);
    TEST_ASSERT_EQUAL_INT16(0, area.x);
    TEST_ASSERT_EQUAL_INT16(top, area.y);
    TEST_ASSERT_EQUAL_INT16(320, area.width);
    TEST_ASSERT_EQUAL_INT16(16, area.height);
    coveredPixels += static_cast<int32_t>(area.width) * area.height;
  }
  TEST_ASSERT_EQUAL_INT32(320 * 240, coveredPixels);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_clips_a_shape_to_the_current_band);
  RUN_TEST(test_rejects_shapes_outside_the_band_or_screen);
  RUN_TEST(test_full_screen_is_covered_once_by_fifteen_bands);
  return UNITY_END();
}
