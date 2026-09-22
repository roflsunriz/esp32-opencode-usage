#include <unity.h>

#include "backlight_timer.h"

void test_accepts_legacy_choices_and_slider_range() {
  // Legacy 3x3 grid choices stay valid so stored records keep working.
  const uint32_t expected[] = {15, 30, 60, 120, 300, 600, 1800, 3600, 7200};
  TEST_ASSERT_EQUAL_UINT32(9, backlight_timer::kTimeoutOptionCount);
  for (const uint32_t timeout : expected) {
    TEST_ASSERT_TRUE(backlight_timer::isSupportedTimeout(timeout));
  }
  // New sliders: 0 (always on) through 24h59m in any second value.
  TEST_ASSERT_TRUE(backlight_timer::isSupportedTimeout(0));
  TEST_ASSERT_TRUE(backlight_timer::isSupportedTimeout(1));
  TEST_ASSERT_TRUE(backlight_timer::isSupportedTimeout(150));
  TEST_ASSERT_TRUE(backlight_timer::isSupportedTimeout(89940));
  TEST_ASSERT_FALSE(backlight_timer::isSupportedTimeout(89941));
  TEST_ASSERT_FALSE(backlight_timer::isSupportedTimeout(UINT32_MAX));
}

void test_sleep_parts_round_trip() {
  TEST_ASSERT_EQUAL_UINT32(0, backlight_timer::sleepTimeoutFromParts(0, 0));
  TEST_ASSERT_EQUAL_UINT32(60, backlight_timer::sleepTimeoutFromParts(1, 0));
  TEST_ASSERT_EQUAL_UINT32(3600, backlight_timer::sleepTimeoutFromParts(0, 1));
  TEST_ASSERT_EQUAL_UINT32(
      89940, backlight_timer::sleepTimeoutFromParts(59, 24));
  TEST_ASSERT_EQUAL_UINT32(
      89940, backlight_timer::sleepTimeoutFromParts(99, 99));
  TEST_ASSERT_EQUAL_UINT32(0, backlight_timer::sleepMinutesPart(7200));
  TEST_ASSERT_EQUAL_UINT32(2, backlight_timer::sleepHoursPart(7200));
  TEST_ASSERT_EQUAL_UINT32(59, backlight_timer::sleepMinutesPart(89940));
  TEST_ASSERT_EQUAL_UINT32(24, backlight_timer::sleepHoursPart(89940));
  TEST_ASSERT_TRUE(backlight_timer::isValidPollSlider(60));
  TEST_ASSERT_TRUE(backlight_timer::isValidPollSlider(600));
  TEST_ASSERT_FALSE(backlight_timer::isValidPollSlider(59));
  TEST_ASSERT_FALSE(backlight_timer::isValidPollSlider(61));
  TEST_ASSERT_FALSE(backlight_timer::isValidPollSlider(601));
}

void test_turns_off_at_the_exact_timeout_boundary() {
  backlight_timer::Model timer;
  timer.start(15, 1000);
  TEST_ASSERT_TRUE(timer.isOn());
  TEST_ASSERT_EQUAL_UINT32(15000, timer.remainingMs(1000));
  TEST_ASSERT_FALSE(timer.tick(15999));
  TEST_ASSERT_TRUE(timer.isOn());
  TEST_ASSERT_EQUAL_UINT32(1, timer.remainingMs(15999));
  TEST_ASSERT_TRUE(timer.tick(16000));
  TEST_ASSERT_FALSE(timer.isOn());
  TEST_ASSERT_EQUAL_UINT32(0, timer.remainingMs(16000));
}

void test_wraparound_keeps_the_deadline_correct() {
  backlight_timer::Model timer;
  const uint32_t start = UINT32_MAX - 5000;
  timer.start(15, start);
  TEST_ASSERT_FALSE(timer.tick(9998));
  TEST_ASSERT_TRUE(timer.isOn());
  TEST_ASSERT_TRUE(timer.tick(9999));
  TEST_ASSERT_FALSE(timer.isOn());
}

void test_wake_and_timeout_change_reset_the_deadline() {
  backlight_timer::Model timer;
  timer.start(15, 0);
  TEST_ASSERT_TRUE(timer.tick(15000));
  TEST_ASSERT_TRUE(timer.wake(20000));
  TEST_ASSERT_TRUE(timer.isOn());
  TEST_ASSERT_FALSE(timer.tick(34999));
  TEST_ASSERT_FALSE(timer.setTimeout(30, 35000));
  TEST_ASSERT_EQUAL_UINT32(30, timer.timeoutSec());
  TEST_ASSERT_FALSE(timer.tick(64999));
  TEST_ASSERT_TRUE(timer.tick(65000));
}

void test_network_updates_do_not_extend_the_timeout() {
  backlight_timer::Model timer;
  timer.start(15, 0);
  // A usage frame may redraw the LCD, but it must not call wake().
  TEST_ASSERT_FALSE(timer.tick(14999));
  TEST_ASSERT_TRUE(timer.tick(15000));
}

void setUp() {}
void tearDown() {}

int runTests() {
  UNITY_BEGIN();
  RUN_TEST(test_accepts_legacy_choices_and_slider_range);
  RUN_TEST(test_sleep_parts_round_trip);
  RUN_TEST(test_turns_off_at_the_exact_timeout_boundary);
  RUN_TEST(test_wraparound_keeps_the_deadline_correct);
  RUN_TEST(test_wake_and_timeout_change_reset_the_deadline);
  RUN_TEST(test_network_updates_do_not_extend_the_timeout);
  return UNITY_END();
}

#ifdef ARDUINO
void setup() { runTests(); }
void loop() {}
#else
int main() { return runTests(); }
#endif
