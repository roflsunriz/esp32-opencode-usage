#include <cstring>

#include <unity.h>

#include "Preferences.h"

uint8_t Preferences::storage_[Preferences::kCapacity] = {};
size_t Preferences::length_ = 0;

bool Preferences::begin(const char * /*name*/, bool /*readOnly*/) {
  return true;
}

size_t Preferences::getBytesLength(const char * /*key*/) const {
  return length_;
}

size_t Preferences::getBytes(const char * /*key*/, void *value,
                             size_t length) const {
  if (value == nullptr || length != length_) {
    return 0;
  }
  memcpy(value, storage_, length_);
  return length_;
}

size_t Preferences::putBytes(const char * /*key*/, const void *value,
                             size_t length) {
  if (value == nullptr || length > kCapacity) {
    return 0;
  }
  memcpy(storage_, value, length);
  length_ = length;
  return length;
}

bool Preferences::remove(const char * /*key*/) {
  clear();
  return true;
}

void Preferences::setFixture(const void *value, size_t length) {
  length_ = 0;
  if (value != nullptr && length <= kCapacity) {
    memcpy(storage_, value, length);
    length_ = length;
  }
}

const uint8_t *Preferences::data() { return storage_; }
size_t Preferences::size() { return length_; }
void Preferences::clear() {
  memset(storage_, 0, sizeof(storage_));
  length_ = 0;
}

#include "../../src/config_store.cpp"

namespace {

struct LegacyV2 {
  uint32_t schemaVersion;
  uint8_t enabled;
  uint8_t reserved[3];
  char ssid[config_store::kSsidCapacity];
  char password[config_store::kPasswordCapacity];
  char authCookie[config_store::kAuthCookieCapacity];
  char workspace[config_store::kWorkspaceCapacity];
  char queryId[config_store::kQueryIdCapacity];
  uint32_t pollIntervalSec;
  uint32_t checksum;
};

struct CurrentV3 {
  uint32_t schemaVersion;
  uint8_t enabled;
  uint8_t reserved[3];
  char ssid[config_store::kSsidCapacity];
  char password[config_store::kPasswordCapacity];
  char authCookie[config_store::kAuthCookieCapacity];
  char workspace[config_store::kWorkspaceCapacity];
  char queryId[config_store::kQueryIdCapacity];
  uint32_t pollIntervalSec;
  uint32_t backlightTimeoutSec;
  uint32_t checksum;
};

uint32_t crc32(const uint8_t *bytes, size_t length) {
  uint32_t value = 0xFFFFFFFFU;
  for (size_t index = 0; index < length; ++index) {
    value ^= bytes[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      value = (value & 1U) != 0U ? (value >> 1U) ^ 0xEDB88320U : (value >> 1U);
    }
  }
  return ~value;
}

LegacyV2 validLegacy() {
  LegacyV2 legacy = {};
  legacy.schemaVersion = 2;
  legacy.enabled = 1;
  snprintf(legacy.ssid, sizeof(legacy.ssid), "test-network");
  snprintf(legacy.password, sizeof(legacy.password), "test-password");
  snprintf(legacy.authCookie, sizeof(legacy.authCookie), "auth=test-cookie");
  snprintf(legacy.workspace, sizeof(legacy.workspace), "wrk_0123456789ABCDEF");
  memset(legacy.queryId, 'a', sizeof(legacy.queryId) - 1);
  legacy.pollIntervalSec = 60;
  legacy.checksum = crc32(reinterpret_cast<const uint8_t *>(&legacy),
                          offsetof(LegacyV2, checksum));
  return legacy;
}

void test_migrates_verified_schema2_and_preserves_credentials() {
  const LegacyV2 legacy = validLegacy();
  Preferences::setFixture(&legacy, sizeof(legacy));
  static config_store::WiFiConfig config;
  config_store::ConfigStore store;
  TEST_ASSERT_TRUE(store.load(config));
  TEST_ASSERT_EQUAL_STRING("test-network", config.ssid);
  TEST_ASSERT_EQUAL_STRING("test-password", config.password);
  TEST_ASSERT_EQUAL_STRING("auth=test-cookie", config.authCookie);
  TEST_ASSERT_EQUAL_STRING("wrk_0123456789ABCDEF", config.workspace);
  TEST_ASSERT_EQUAL_UINT32(60, config.pollIntervalSec);
  TEST_ASSERT_EQUAL_UINT32(config_store::kDefaultBacklightTimeoutSec,
                           config.backlightTimeoutSec);

  TEST_ASSERT_EQUAL_UINT32(sizeof(CurrentV3), Preferences::size());
  const auto *saved = reinterpret_cast<const CurrentV3 *>(Preferences::data());
  TEST_ASSERT_EQUAL_UINT32(config_store::kConfigSchemaVersion,
                           saved->schemaVersion);
  TEST_ASSERT_EQUAL_UINT32(config_store::kDefaultBacklightTimeoutSec,
                           saved->backlightTimeoutSec);
  TEST_ASSERT_EQUAL_UINT32(crc32(reinterpret_cast<const uint8_t *>(saved),
                                 offsetof(CurrentV3, checksum)),
                           saved->checksum);
  TEST_ASSERT_EQUAL_STRING(legacy.authCookie, saved->authCookie);
  TEST_ASSERT_EQUAL_STRING(legacy.workspace, saved->workspace);
  TEST_ASSERT_EQUAL_STRING(legacy.queryId, saved->queryId);
}

void test_rejects_a_schema2_record_with_invalid_crc() {
  LegacyV2 legacy = validLegacy();
  legacy.checksum ^= 0x01U;
  Preferences::setFixture(&legacy, sizeof(legacy));
  static config_store::WiFiConfig config;
  config_store::ConfigStore store;
  TEST_ASSERT_FALSE(store.load(config));
  TEST_ASSERT_EQUAL_UINT32(0, Preferences::size());
}

void test_loads_display_setting_from_a_valid_disabled_wifi_record() {
  CurrentV3 saved = {};
  saved.schemaVersion = config_store::kConfigSchemaVersion;
  saved.enabled = 0;
  saved.pollIntervalSec = config_store::kDefaultPollIntervalSec;
  saved.backlightTimeoutSec = 15;
  saved.checksum = crc32(reinterpret_cast<const uint8_t *>(&saved),
                         offsetof(CurrentV3, checksum));
  Preferences::setFixture(&saved, sizeof(saved));
  static config_store::WiFiConfig config;
  config_store::ConfigStore store;
  TEST_ASSERT_FALSE(store.load(config));
  TEST_ASSERT_FALSE(config.enabled);
  TEST_ASSERT_EQUAL_UINT32(15, config.backlightTimeoutSec);
}

} // namespace

void setUp() { Preferences::clear(); }
void tearDown() {}

int runTests() {
  UNITY_BEGIN();
  RUN_TEST(test_migrates_verified_schema2_and_preserves_credentials);
  RUN_TEST(test_rejects_a_schema2_record_with_invalid_crc);
  RUN_TEST(test_loads_display_setting_from_a_valid_disabled_wifi_record);
  return UNITY_END();
}

int main() { return runTests(); }
