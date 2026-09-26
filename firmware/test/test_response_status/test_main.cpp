#include <cstring>

#include <unity.h>

// Pure response-status helpers shared with the production transport.
#include "../../src/opencode_parser.cpp"
#include "../../src/opencode_status.cpp"

namespace {

// 2026-09-26 console shape: extra top-level and access keys must be ignored.
constexpr char kLiveShape[] =
    "{\"subscriberUserId\":\"acc_XXXXXXXXXXXXXXXXXXXXXXXXXX\","
    "\"product\":\"go\",\"renewalProduct\":\"go\","
    "\"paymentMethodId\":\"XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX\","
    "\"renewalCurrency\":\"USD\",\"useBalance\":false,"
    "\"cancelAtPeriodEnd\":false,\"renewalPending\":false,"
    "\"access\":{\"startsAt\":\"2026-09-02T22:58:39.000Z\","
    "\"endsAt\":\"2026-10-02T22:58:39.000Z\",\"cancelAtPeriodEnd\":false,"
    "\"meters\":{\"fiveHour\":{\"startsAt\":\"2026-09-26T10:05:04.620Z\","
    "\"resetsAt\":\"2026-09-26T15:05:04.620Z\",\"limitMicroCents\":"
    "\"1200000000\",\"usedMicroCents\":\"29216144\"},"
    "\"week\":{\"startsAt\":\"2026-09-21T00:00:00.000Z\","
    "\"resetsAt\":\"2026-09-28T00:00:00.000Z\",\"limitMicroCents\":"
    "\"3000000000\",\"usedMicroCents\":\"1759761639\"},"
    "\"month\":{\"limitMicroCents\":\"6000000000\",\"usedMicroCents\":"
    "\"5737024167\"}}},"
    "\"upgradePrice\":{\"amountMicroCents\":\"1000000000\","
    "\"currency\":\"USD\"}}";

constexpr uint64_t kLiveUpdatedAt = 1790424987ULL;

void test_status_text_distinguishes_too_large_from_invalid() {
  using opencode_client::Result;
  TEST_ASSERT_EQUAL_STRING(
      "OpenCode response too large",
      opencode_client::responseStatusText(Result::kPayloadTooLarge));
  TEST_ASSERT_EQUAL_STRING(
      "OpenCode response invalid",
      opencode_client::responseStatusText(Result::kResponseInvalid));
  TEST_ASSERT_EQUAL_STRING(
      "OpenCode auth failed - login",
      opencode_client::responseStatusText(Result::kAuthenticationRequired));
  TEST_ASSERT_EQUAL_STRING(
      "OpenCode query invalid",
      opencode_client::responseStatusText(Result::kQueryRejected));
  TEST_ASSERT_EQUAL_STRING(
      "OpenCode HTTPS failed",
      opencode_client::responseStatusText(Result::kTransportFailed));
  TEST_ASSERT_EQUAL_STRING(
      "OpenCode updated",
      opencode_client::responseStatusText(Result::kOk));
}

void test_truncation_needs_a_declared_length() {
  TEST_ASSERT_FALSE(opencode_client::isTruncatedBody(796, 796));
  TEST_ASSERT_TRUE(opencode_client::isTruncatedBody(796, 100));
  TEST_ASSERT_TRUE(opencode_client::isTruncatedBody(796, 0));
  // Chunked or length-less responses carry -1; framing errors there already
  // surface as transport failures inside HTTPClient.
  TEST_ASSERT_FALSE(opencode_client::isTruncatedBody(-1, 100));
  TEST_ASSERT_FALSE(opencode_client::isTruncatedBody(0, 0));
}

void test_diagnostic_reports_only_sizes_and_flags() {
  opencode_client::ResponseDiagnostic diagnostic;
  diagnostic.httpCode = 200;
  diagnostic.contentLength = 796;
  diagnostic.bodyBytes = 100;
  diagnostic.contentTypeLength = 16;
  diagnostic.isJson = true;
  diagnostic.isHtml = false;
  char frame[192] = {};
  const size_t written = opencode_client::formatResponseDiagnostic(
      frame, sizeof(frame), "invalid", diagnostic);
  TEST_ASSERT_TRUE(written > 0);
  TEST_ASSERT_EQUAL_STRING(
      "{\"version\":1,\"type\":\"diagnostic\",\"component\":"
      "\"opencode_response\",\"result\":\"invalid\",\"httpCode\":200,"
      "\"contentLength\":796,\"bodyBytes\":100,\"contentTypeLength\":16,"
      "\"isJson\":true,\"isHtml\":false}",
      frame);
}

void test_diagnostic_rejects_bad_result_and_small_buffers() {
  opencode_client::ResponseDiagnostic diagnostic;
  char frame[192] = {};
  TEST_ASSERT_EQUAL_UINT(0, opencode_client::formatResponseDiagnostic(
                                frame, sizeof(frame), "", diagnostic));
  TEST_ASSERT_EQUAL_UINT(0, opencode_client::formatResponseDiagnostic(
                                frame, sizeof(frame), "INVALID", diagnostic));
  TEST_ASSERT_EQUAL_UINT(0, opencode_client::formatResponseDiagnostic(
                                frame, sizeof(frame), "way_too_long_result",
                                diagnostic));
  TEST_ASSERT_EQUAL_STRING("", frame);
  char tiny[16] = {};
  TEST_ASSERT_EQUAL_UINT(0, opencode_client::formatResponseDiagnostic(
                                tiny, sizeof(tiny), "invalid", diagnostic));
  TEST_ASSERT_EQUAL_STRING("", tiny);
  TEST_ASSERT_EQUAL_UINT(
      0, opencode_client::formatResponseDiagnostic(nullptr, 0, "invalid",
                                                   diagnostic));
}

void test_live_console_shape_still_parses() {
  char payload[opencode_client::kUsagePayloadCapacity] = {};
  TEST_ASSERT_TRUE(opencode_client::normalizeConsoleStatusUsage(
      kLiveShape, strlen(kLiveShape), kLiveUpdatedAt, payload,
      sizeof(payload)));
  TEST_ASSERT_NOT_NULL(strstr(payload, "\"used\":0.29216144"));
  TEST_ASSERT_NOT_NULL(strstr(payload, "\"used\":17.59761639"));
  TEST_ASSERT_NOT_NULL(strstr(payload, "\"used\":57.37024167"));
  TEST_ASSERT_NOT_NULL(strstr(payload, "\"resetInSec\":10117"));
  TEST_ASSERT_NOT_NULL(strstr(payload, "\"resetInSec\":128613"));
  TEST_ASSERT_NOT_NULL(strstr(payload, "\"resetInSec\":556932"));
}

} // namespace

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_status_text_distinguishes_too_large_from_invalid);
  RUN_TEST(test_truncation_needs_a_declared_length);
  RUN_TEST(test_diagnostic_reports_only_sizes_and_flags);
  RUN_TEST(test_diagnostic_rejects_bad_result_and_small_buffers);
  RUN_TEST(test_live_console_shape_still_parses);
  return UNITY_END();
}
