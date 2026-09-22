#include <cstring>

#include <unity.h>

#include "Preferences.h"

uint8_t Preferences::storage_[Preferences::kCapacity] = {};
size_t Preferences::length_ = 0;
bool Preferences::writeFailure_ = false;

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
  if (writeFailure_ || value == nullptr || length > kCapacity) {
    return 0;
  }
  memcpy(storage_, value, length);
  length_ = length;
  return length;
}

void Preferences::setWriteFailure(bool enabled) { writeFailure_ = enabled; }

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
  writeFailure_ = false;
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

struct LegacyV3 {
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

struct CurrentV4 {
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

static_assert(sizeof(LegacyV3) == sizeof(CurrentV4),
              "LegacyV3 and CurrentV4 must have the same size");

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
  snprintf(legacy.authCookie, sizeof(legacy.authCookie),
           "__Host-console_session=test-cookie");
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
  TEST_ASSERT_EQUAL_STRING("__Host-console_session=test-cookie",
                             config.authCookie);
  TEST_ASSERT_EQUAL_STRING("wrk_0123456789ABCDEF", config.workspace);
  TEST_ASSERT_EQUAL_UINT32(60, config.pollIntervalSec);
  TEST_ASSERT_EQUAL_UINT32(config_store::kDefaultBacklightTimeoutSec,
                           config.backlightTimeoutSec);

  TEST_ASSERT_FALSE(config.screenFlipped);
  TEST_ASSERT_EQUAL_UINT32(sizeof(CurrentV4), Preferences::size());
  const auto *saved = reinterpret_cast<const CurrentV4 *>(Preferences::data());
  TEST_ASSERT_EQUAL_UINT32(config_store::kConfigSchemaVersion,
                           saved->schemaVersion);
  TEST_ASSERT_EQUAL_UINT32(config_store::kDefaultBacklightTimeoutSec,
                           saved->backlightTimeoutSec);
  TEST_ASSERT_EQUAL_UINT8(0, saved->reserved[0]);
  TEST_ASSERT_EQUAL_UINT32(crc32(reinterpret_cast<const uint8_t *>(saved),
                                 offsetof(CurrentV4, checksum)),
                           saved->checksum);
  TEST_ASSERT_EQUAL_STRING(legacy.authCookie, saved->authCookie);
  TEST_ASSERT_EQUAL_STRING(legacy.workspace, saved->workspace);
  TEST_ASSERT_EQUAL_STRING(legacy.queryId, saved->queryId);
}

LegacyV3 validLegacyV3() {
  LegacyV3 legacy = {};
  legacy.schemaVersion = 3;
  legacy.enabled = 1;
  snprintf(legacy.ssid, sizeof(legacy.ssid), "test-network");
  snprintf(legacy.password, sizeof(legacy.password), "test-password");
  snprintf(legacy.authCookie, sizeof(legacy.authCookie),
           "__Host-console_session=test-cookie");
  snprintf(legacy.workspace, sizeof(legacy.workspace), "wrk_0123456789ABCDEF");
  memset(legacy.queryId, 'a', sizeof(legacy.queryId) - 1);
  legacy.pollIntervalSec = 120;
  legacy.backlightTimeoutSec = 300;
  legacy.checksum = crc32(reinterpret_cast<const uint8_t *>(&legacy),
                          offsetof(LegacyV3, checksum));
  return legacy;
}

void test_migrates_verified_schema3_and_preserves_timeout() {
  const LegacyV3 legacy = validLegacyV3();
  Preferences::setFixture(&legacy, sizeof(legacy));
  static config_store::WiFiConfig config;
  config_store::ConfigStore store;
  TEST_ASSERT_TRUE(store.load(config));
  TEST_ASSERT_EQUAL_STRING(legacy.authCookie, config.authCookie);
  TEST_ASSERT_EQUAL_STRING(legacy.workspace, config.workspace);
  TEST_ASSERT_EQUAL_UINT32(legacy.pollIntervalSec, config.pollIntervalSec);
  TEST_ASSERT_EQUAL_UINT32(legacy.backlightTimeoutSec,
                           config.backlightTimeoutSec);
  TEST_ASSERT_FALSE(config.screenFlipped);

  TEST_ASSERT_EQUAL_UINT32(sizeof(CurrentV4), Preferences::size());
  const auto *saved = reinterpret_cast<const CurrentV4 *>(Preferences::data());
  TEST_ASSERT_EQUAL_UINT32(config_store::kConfigSchemaVersion,
                           saved->schemaVersion);
  TEST_ASSERT_EQUAL_UINT8(0, saved->reserved[0]);
  TEST_ASSERT_EQUAL_UINT32(legacy.backlightTimeoutSec,
                           saved->backlightTimeoutSec);
  TEST_ASSERT_EQUAL_UINT32(crc32(reinterpret_cast<const uint8_t *>(saved),
                                 offsetof(CurrentV4, checksum)),
                           saved->checksum);
}

void test_schema3_migration_write_failure_preserves_legacy_until_next_load() {
  const LegacyV3 legacy = validLegacyV3();
  Preferences::setFixture(&legacy, sizeof(legacy));
  Preferences::setWriteFailure(true);

  config_store::WiFiConfig config;
  config_store::ConfigStore store;
  TEST_ASSERT_TRUE(store.load(config));
  TEST_ASSERT_EQUAL_STRING(legacy.authCookie, config.authCookie);
  TEST_ASSERT_EQUAL_UINT32(legacy.backlightTimeoutSec,
                           config.backlightTimeoutSec);
  TEST_ASSERT_FALSE(config.screenFlipped);
  TEST_ASSERT_EQUAL_UINT32(sizeof(legacy), Preferences::size());
  TEST_ASSERT_EQUAL_MEMORY(&legacy, Preferences::data(), sizeof(legacy));

  Preferences::setWriteFailure(false);
  config_store::WiFiConfig nextConfig;
  config_store::ConfigStore nextStore;
  TEST_ASSERT_TRUE(nextStore.load(nextConfig));
  TEST_ASSERT_EQUAL_STRING(legacy.authCookie, nextConfig.authCookie);
  TEST_ASSERT_EQUAL_UINT32(legacy.backlightTimeoutSec,
                           nextConfig.backlightTimeoutSec);
  TEST_ASSERT_FALSE(nextConfig.screenFlipped);
  TEST_ASSERT_EQUAL_UINT32(sizeof(CurrentV4), Preferences::size());
  const auto *migrated = reinterpret_cast<const CurrentV4 *>(Preferences::data());
  TEST_ASSERT_EQUAL_UINT32(config_store::kConfigSchemaVersion,
                           migrated->schemaVersion);
  TEST_ASSERT_EQUAL_UINT8(0, migrated->reserved[0]);
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
  CurrentV4 saved = {};
  saved.schemaVersion = config_store::kConfigSchemaVersion;
  saved.enabled = 0;
  saved.reserved[0] = 1;
  saved.pollIntervalSec = config_store::kDefaultPollIntervalSec;
  saved.backlightTimeoutSec = 15;
  saved.checksum = crc32(reinterpret_cast<const uint8_t *>(&saved),
                         offsetof(CurrentV4, checksum));
  Preferences::setFixture(&saved, sizeof(saved));
  static config_store::WiFiConfig config;
  config_store::ConfigStore store;
  TEST_ASSERT_FALSE(store.load(config));
  TEST_ASSERT_FALSE(config.enabled);
  TEST_ASSERT_TRUE(config.screenFlipped);
  TEST_ASSERT_EQUAL_UINT32(15, config.backlightTimeoutSec);
}

void test_rejects_current_record_with_invalid_flip_value() {
  CurrentV4 saved = {};
  saved.schemaVersion = config_store::kConfigSchemaVersion;
  saved.enabled = 0;
  saved.reserved[0] = 2;
  saved.pollIntervalSec = config_store::kDefaultPollIntervalSec;
  saved.backlightTimeoutSec = config_store::kDefaultBacklightTimeoutSec;
  saved.checksum = crc32(reinterpret_cast<const uint8_t *>(&saved),
                         offsetof(CurrentV4, checksum));
  Preferences::setFixture(&saved, sizeof(saved));
  static config_store::WiFiConfig config;
  config_store::ConfigStore store;
  TEST_ASSERT_FALSE(store.load(config));
  TEST_ASSERT_EQUAL_UINT32(0, Preferences::size());
}

void test_rejects_legacy_auth_cookie_scheme() {
  // The console no longer accepts the legacy auth cookie. A stored record
  // with the old scheme must be discarded so the setup screen asks for a
  // fresh login instead of polling with a dead credential.
  CurrentV4 saved = {};
  saved.schemaVersion = config_store::kConfigSchemaVersion;
  saved.enabled = 1;
  snprintf(saved.ssid, sizeof(saved.ssid), "test-network");
  snprintf(saved.authCookie, sizeof(saved.authCookie), "auth=test-cookie");
  snprintf(saved.workspace, sizeof(saved.workspace), "wrk_0123456789ABCDEF");
  saved.pollIntervalSec = config_store::kDefaultPollIntervalSec;
  saved.backlightTimeoutSec = config_store::kDefaultBacklightTimeoutSec;
  saved.checksum = crc32(reinterpret_cast<const uint8_t *>(&saved),
                         offsetof(CurrentV4, checksum));
  Preferences::setFixture(&saved, sizeof(saved));
  static config_store::WiFiConfig config;
  config_store::ConfigStore store;
  TEST_ASSERT_FALSE(store.load(config));
  TEST_ASSERT_EQUAL_UINT32(0, Preferences::size());
}

void test_save_persists_screen_flip() {
  config_store::WiFiConfig config;
  config_store::reset(config);
  config.screenFlipped = true;
  char error[96] = {};
  config_store::ConfigStore store;
  TEST_ASSERT_TRUE(store.save(config, error, sizeof(error)));
  TEST_ASSERT_EQUAL_UINT32(sizeof(CurrentV4), Preferences::size());
  const auto *saved = reinterpret_cast<const CurrentV4 *>(Preferences::data());
  TEST_ASSERT_EQUAL_UINT8(1, saved->reserved[0]);
  TEST_ASSERT_EQUAL_UINT32(crc32(reinterpret_cast<const uint8_t *>(saved),
                                 offsetof(CurrentV4, checksum)),
                           saved->checksum);
}

void test_save_reports_preferences_write_failure() {
  config_store::WiFiConfig config;
  config_store::reset(config);
  char error[96] = {};
  Preferences::setWriteFailure(true);
  config_store::ConfigStore store;
  TEST_ASSERT_FALSE(store.save(config, error, sizeof(error)));
  TEST_ASSERT_EQUAL_STRING("Preferences write failed", error);
  TEST_ASSERT_EQUAL_UINT32(0, Preferences::size());
}

void test_failed_false_flip_save_preserves_existing_true_flip() {
  config_store::WiFiConfig initial;
  config_store::reset(initial);
  initial.screenFlipped = true;
  char error[96] = {};
  config_store::ConfigStore store;
  TEST_ASSERT_TRUE(store.save(initial, error, sizeof(error)));

  CurrentV4 before = {};
  memcpy(&before, Preferences::data(), sizeof(before));

  config_store::WiFiConfig attempted = initial;
  attempted.screenFlipped = false;
  Preferences::setWriteFailure(true);
  memset(error, 0, sizeof(error));
  TEST_ASSERT_FALSE(store.save(attempted, error, sizeof(error)));
  TEST_ASSERT_EQUAL_STRING("Preferences write failed", error);
  TEST_ASSERT_EQUAL_UINT32(sizeof(before), Preferences::size());
  TEST_ASSERT_EQUAL_MEMORY(&before, Preferences::data(), sizeof(before));

  Preferences::setWriteFailure(false);
  config_store::WiFiConfig reloaded;
  config_store::ConfigStore reloadStore;
  TEST_ASSERT_FALSE(reloadStore.load(reloaded));
  TEST_ASSERT_TRUE(reloaded.screenFlipped);
}

} // namespace

void setUp() { Preferences::clear(); }
void tearDown() {}

int runTests() {
  UNITY_BEGIN();
  RUN_TEST(test_migrates_verified_schema2_and_preserves_credentials);
  RUN_TEST(test_migrates_verified_schema3_and_preserves_timeout);
  RUN_TEST(test_schema3_migration_write_failure_preserves_legacy_until_next_load);
  RUN_TEST(test_rejects_a_schema2_record_with_invalid_crc);
  RUN_TEST(test_loads_display_setting_from_a_valid_disabled_wifi_record);
  RUN_TEST(test_rejects_current_record_with_invalid_flip_value);
  RUN_TEST(test_rejects_legacy_auth_cookie_scheme);
  RUN_TEST(test_save_persists_screen_flip);
  RUN_TEST(test_save_reports_preferences_write_failure);
  RUN_TEST(test_failed_false_flip_save_preserves_existing_true_flip);
  return UNITY_END();
}

int main() { return runTests(); }
