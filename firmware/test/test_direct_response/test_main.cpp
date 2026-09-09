#include <cstring>

#include <unity.h>

// This source is the strict parser linked into the production transport.
#include "../../src/opencode_parser.cpp"

namespace {

constexpr char kResponse[] =
    ";0x000001ff;(($R)=>({rollingUsage:$R[2]={status:\"ok\","
    "resetInSec:18000,usagePercent:0,usage:0,limit:1200000000},"
    "weeklyUsage:$R[3]={status:\"ok\",resetInSec:400000,"
    "usagePercent:0.8,usage:24876543,limit:3000000000},"
    "monthlyUsage:$R[4]={status:\"ok\",resetInSec:2000000,"
    "usagePercent:44,usage:2640123456,limit:6000000000}}))($R);"
    "throw Error('the parser must not execute this');";

void test_normalizes_all_three_seroval_windows_without_evaluation() {
  char payload[opencode_client::kUsagePayloadCapacity] = {};
  TEST_ASSERT_TRUE(opencode_client::normalizeSerovalUsage(
      kResponse, strlen(kResponse), 1770000000, payload, sizeof(payload)));
  TEST_ASSERT_NOT_NULL(strstr(payload, "\"version\":1"));
  TEST_ASSERT_NOT_NULL(strstr(payload, "\"updatedAt\":1770000000"));
  TEST_ASSERT_NOT_NULL(strstr(payload, "\"used\":0.24876543"));
  TEST_ASSERT_NOT_NULL(strstr(payload, "\"percent\":0.8"));
  TEST_ASSERT_NOT_NULL(strstr(payload, "\"resetInSec\":2000000"));
}

void test_rejects_incomplete_duplicate_or_inconsistent_windows() {
  char payload[opencode_client::kUsagePayloadCapacity] = {};
  constexpr char kIncomplete[] =
      "rollingUsage:{usage:1,limit:2,usagePercent:50,resetInSec:1}";
  TEST_ASSERT_FALSE(opencode_client::normalizeSerovalUsage(
      kIncomplete, strlen(kIncomplete), 1, payload, sizeof(payload)));

  char duplicate[sizeof(kResponse) + 160] = {};
  snprintf(duplicate, sizeof(duplicate),
           "%s,rollingUsage:{usage:1,limit:2,"
           "usagePercent:50,resetInSec:1}",
           kResponse);
  TEST_ASSERT_FALSE(opencode_client::normalizeSerovalUsage(
      duplicate, strlen(duplicate), 1, payload, sizeof(payload)));

  char inconsistent[sizeof(kResponse)] = {};
  snprintf(inconsistent, sizeof(inconsistent), "%s", kResponse);
  char *percent = strstr(inconsistent, "usagePercent:0.8");
  TEST_ASSERT_NOT_NULL(percent);
  memcpy(percent, "usagePercent:99 ", strlen("usagePercent:99 "));
  TEST_ASSERT_FALSE(opencode_client::normalizeSerovalUsage(
      inconsistent, strlen(inconsistent), 1, payload, sizeof(payload)));
}

void test_extracts_only_a_single_lowercase_query_identifier() {
  const char query[] =
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  char script[256] = {};
  char result[65] = {};
  snprintf(script, sizeof(script),
           "const queryLiteSubscription_query = createServerReference('%s');",
           query);
  TEST_ASSERT_TRUE(opencode_client::extractQueryIdFromScript(
      script, strlen(script), result, sizeof(result)));
  TEST_ASSERT_EQUAL_STRING(query, result);

  snprintf(script, sizeof(script),
           "const queryLiteSubscription_query = f('%s');"
           "const queryLiteSubscription_query = f('%s');",
           query, query);
  TEST_ASSERT_FALSE(opencode_client::extractQueryIdFromScript(
      script, strlen(script), result, sizeof(result)));
}

} // namespace

void setUp() {}
void tearDown() {}

int runTests() {
  UNITY_BEGIN();
  RUN_TEST(test_normalizes_all_three_seroval_windows_without_evaluation);
  RUN_TEST(test_rejects_incomplete_duplicate_or_inconsistent_windows);
  RUN_TEST(test_extracts_only_a_single_lowercase_query_identifier);
  return UNITY_END();
}

int main() { return runTests(); }
