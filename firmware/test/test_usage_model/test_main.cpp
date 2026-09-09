#include <cmath>

#include <unity.h>

#include "usage_model.h"

using usage_model::PeriodUsage;

void test_period_accepts_explicit_percent_and_reset() {
  PeriodUsage period;
  TEST_ASSERT_TRUE(
      usage_model::setPeriod(period, 3.25, 12.0, 27.1, true, 1800, true));
  TEST_ASSERT_TRUE(period.valid);
  TEST_ASSERT_DOUBLE_WITHIN(0.001, 3.25, period.used);
  TEST_ASSERT_DOUBLE_WITHIN(0.001, 27.1, period.percent);
  TEST_ASSERT_EQUAL_INT64(1800, period.resetInSec);
}

void test_period_derives_percent_and_clamps_bar_only() {
  PeriodUsage period;
  TEST_ASSERT_TRUE(
      usage_model::setPeriod(period, 15.0, 12.0, 0.0, false, -1, false));
  TEST_ASSERT_DOUBLE_WITHIN(0.001, 125.0, period.percent);
  TEST_ASSERT_EQUAL_UINT16(100, usage_model::filledWidth(period.percent, 100));
  TEST_ASSERT_EQUAL_UINT16(50, usage_model::filledWidth(50.0, 100));
  TEST_ASSERT_EQUAL_UINT16(0, usage_model::filledWidth(-1.0, 100));
}

void test_period_rejects_invalid_values() {
  PeriodUsage period;
  TEST_ASSERT_FALSE(
      usage_model::setPeriod(period, -0.1, 12.0, 0.0, false, -1, false));
  TEST_ASSERT_FALSE(
      usage_model::setPeriod(period, 1.0, 0.0, 0.0, false, -1, false));
  TEST_ASSERT_FALSE(
      usage_model::setPeriod(period, 1.0, 12.0, NAN, true, -1, false));
  TEST_ASSERT_FALSE(
      usage_model::setPeriod(period, 1.0, 12.0, 1.0, true, -1, true));
}

void test_period_labels_are_stable() {
  TEST_ASSERT_EQUAL_STRING("5H", usage_model::periodLabel(0));
  TEST_ASSERT_EQUAL_STRING("WEEK", usage_model::periodLabel(1));
  TEST_ASSERT_EQUAL_STRING("MONTH", usage_model::periodLabel(2));
  TEST_ASSERT_EQUAL_STRING("?", usage_model::periodLabel(3));
}

void test_staleness_uses_poll_interval_and_unsigned_elapsed_time() {
  TEST_ASSERT_FALSE(usage_model::isStale(179999, 60));
  TEST_ASSERT_TRUE(usage_model::isStale(180000, 60));
  TEST_ASSERT_FALSE(usage_model::isStale(599999, 300));
  TEST_ASSERT_TRUE(usage_model::isStale(600000, 300));
  const uint32_t beforeWrap = UINT32_MAX - 100000;
  const uint32_t afterWrap = 100000;
  TEST_ASSERT_TRUE(usage_model::isStale(afterWrap - beforeWrap, 60));
}

void setUp() {}
void tearDown() {}

int runTests() {
  UNITY_BEGIN();
  RUN_TEST(test_period_accepts_explicit_percent_and_reset);
  RUN_TEST(test_period_derives_percent_and_clamps_bar_only);
  RUN_TEST(test_period_rejects_invalid_values);
  RUN_TEST(test_period_labels_are_stable);
  RUN_TEST(test_staleness_uses_poll_interval_and_unsigned_elapsed_time);
  return UNITY_END();
}

#ifdef ARDUINO
void setup() { runTests(); }
void loop() {}
#else
int main() { return runTests(); }
#endif
