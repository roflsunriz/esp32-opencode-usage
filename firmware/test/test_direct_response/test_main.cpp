#include <cstring>

#include <unity.h>

// This source is the strict parser linked into the production transport.
#include "../../src/opencode_parser.cpp"

namespace {

constexpr char kResponse[] =
    "{\"subscriberUserId\":\"acc_test\",\"access\":{"
    "\"startsAt\":\"2026-09-02T22:58:39.000Z\","
    "\"endsAt\":\"2026-10-02T22:58:39.000Z\","
    "\"meters\":{\"fiveHour\":{\"startsAt\":\"2026-09-22T06:34:36.282Z\","
    "\"resetsAt\":\"2026-09-22T11:34:36.282Z\",\"limitMicroCents\":"
    "\"1200000000\",\"usedMicroCents\":\"3647255\"},"
    "\"week\":{\"startsAt\":\"2026-09-21T00:00:00.000Z\","
    "\"resetsAt\":\"2026-09-28T00:00:00.000Z\",\"limitMicroCents\":"
    "\"3000000000\",\"usedMicroCents\":\"3647255\"},"
    "\"month\":{\"limitMicroCents\":\"6000000000\",\"usedMicroCents\":"
    "\"3980909783\"}}}}";

// 2026-09-22T06:34:36Z in epoch seconds.
constexpr uint64_t kUpdatedAt = 1790058876ULL;

void test_normalizes_all_three_console_meters() {
  char payload[opencode_client::kUsagePayloadCapacity] = {};
  TEST_ASSERT_TRUE(opencode_client::normalizeConsoleStatusUsage(
      kResponse, strlen(kResponse), kUpdatedAt, payload, sizeof(payload)));
  TEST_ASSERT_NOT_NULL(strstr(payload, "\"version\":1"));
  TEST_ASSERT_NOT_NULL(strstr(payload, "\"updatedAt\":1790058876"));
  TEST_ASSERT_NOT_NULL(strstr(payload, "\"used\":0.03647255"));
  TEST_ASSERT_NOT_NULL(strstr(payload, "\"limit\":12.00000000"));
  TEST_ASSERT_NOT_NULL(strstr(payload, "\"resetInSec\":18000"));
  TEST_ASSERT_NOT_NULL(strstr(payload, "\"used\":39.80909783"));
  // The monthly reset comes from access.endsAt: 10d16h24m03s after updatedAt.
  TEST_ASSERT_NOT_NULL(strstr(payload, "\"resetInSec\":923043"));
}

void test_rejects_incomplete_or_malformed_status() {
  char payload[opencode_client::kUsagePayloadCapacity] = {};
  constexpr char kMissingMonth[] =
      "{\"access\":{\"endsAt\":\"2026-10-02T22:58:39.000Z\","
      "\"meters\":{\"fiveHour\":{\"resetsAt\":\"2026-09-22T11:34:36.282Z\","
      "\"limitMicroCents\":\"1200000000\",\"usedMicroCents\":\"1\"},"
      "\"week\":{\"resetsAt\":\"2026-09-28T00:00:00.000Z\","
      "\"limitMicroCents\":\"3000000000\",\"usedMicroCents\":\"1\"}}}}";
  TEST_ASSERT_FALSE(opencode_client::normalizeConsoleStatusUsage(
      kMissingMonth, strlen(kMissingMonth), kUpdatedAt, payload,
      sizeof(payload)));

  char noLimit[sizeof(kResponse)] = {};
  snprintf(noLimit, sizeof(noLimit), "%s", kResponse);
  char *limit = strstr(noLimit, "\"limitMicroCents\":\"6000000000\"");
  TEST_ASSERT_NOT_NULL(limit);
  memcpy(limit, "\"limitMicroCents\":\"0000000000\"",
         strlen("\"limitMicroCents\":\"0000000000\""));
  TEST_ASSERT_FALSE(opencode_client::normalizeConsoleStatusUsage(
      noLimit, strlen(noLimit), kUpdatedAt, payload, sizeof(payload)));

  char badDate[sizeof(kResponse)] = {};
  snprintf(badDate, sizeof(badDate), "%s", kResponse);
  char *reset = strstr(badDate, "2026-09-22T11:34:36.282Z");
  TEST_ASSERT_NOT_NULL(reset);
  memcpy(reset, "2026-13-99T99:99:99.000Z", strlen("2026-13-99T99:99:99.000Z"));
  TEST_ASSERT_FALSE(opencode_client::normalizeConsoleStatusUsage(
      badDate, strlen(badDate), kUpdatedAt, payload, sizeof(payload)));

  // 2026-02-30 does not exist and must not be accepted as a reset time.
  char impossible[sizeof(kResponse)] = {};
  snprintf(impossible, sizeof(impossible), "%s", kResponse);
  char *day = strstr(impossible, "2026-09-22T11:34:36.282Z");
  TEST_ASSERT_NOT_NULL(day);
  memcpy(day, "2026-02-30T11:34:36.282Z", strlen("2026-02-30T11:34:36.282Z"));
  TEST_ASSERT_FALSE(opencode_client::normalizeConsoleStatusUsage(
      impossible, strlen(impossible), kUpdatedAt, payload, sizeof(payload)));
}

} // namespace

void setUp() {}
void tearDown() {}

int runTests() {
  UNITY_BEGIN();
  RUN_TEST(test_normalizes_all_three_console_meters);
  RUN_TEST(test_rejects_incomplete_or_malformed_status);
  return UNITY_END();
}

int main() { return runTests(); }
