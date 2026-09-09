#include "config_store.h"

#include <Preferences.h>

#include <cstddef>
#include <cstdio>
#include <cstring>

#include "backlight_timer.h"

namespace config_store {
namespace {

Preferences preferences;
constexpr char kConfigKey[] = "config";

struct PersistedConfigV2 {
  uint32_t schemaVersion;
  uint8_t enabled;
  uint8_t reserved[3];
  char ssid[kSsidCapacity];
  char password[kPasswordCapacity];
  char authCookie[kAuthCookieCapacity];
  char workspace[kWorkspaceCapacity];
  char queryId[kQueryIdCapacity];
  uint32_t pollIntervalSec;
  uint32_t checksum;
};

struct PersistedConfigV3 {
  uint32_t schemaVersion;
  uint8_t enabled;
  uint8_t reserved[3];
  char ssid[kSsidCapacity];
  char password[kPasswordCapacity];
  char authCookie[kAuthCookieCapacity];
  char workspace[kWorkspaceCapacity];
  char queryId[kQueryIdCapacity];
  uint32_t pollIntervalSec;
  uint32_t backlightTimeoutSec;
  uint32_t checksum;
};

union PersistedStorage {
  PersistedConfigV2 v2;
  PersistedConfigV3 v3;
};

PersistedStorage stored;

static_assert(offsetof(PersistedConfigV2, checksum) + sizeof(uint32_t) ==
                  sizeof(PersistedConfigV2),
              "PersistedConfigV2 must have checksum as its final field");
static_assert(offsetof(PersistedConfigV3, checksum) + sizeof(uint32_t) ==
                  sizeof(PersistedConfigV3),
              "PersistedConfigV3 must have checksum as its final field");

uint32_t checksum(const uint8_t *bytes, size_t length) {
  uint32_t value = 0xFFFFFFFFU;
  for (size_t index = 0; index < length; ++index) {
    value ^= bytes[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      value = (value & 1U) != 0U ? (value >> 1U) ^ 0xEDB88320U : (value >> 1U);
    }
  }
  return ~value;
}

bool hasControlCharacter(const char *value) {
  for (const unsigned char *cursor =
           reinterpret_cast<const unsigned char *>(value);
       *cursor != '\0'; ++cursor) {
    if (*cursor < 0x20 || *cursor == 0x7f) {
      return true;
    }
  }
  return false;
}

void setError(char *error, size_t capacity, const char *message) {
  if (error == nullptr || capacity == 0) {
    return;
  }
  snprintf(error, capacity, "%s", message);
}

template <typename Persisted>
void copyCommonConfig(WiFiConfig &config, const Persisted &stored) {
  config.enabled = stored.enabled != 0;
  memcpy(config.ssid, stored.ssid, sizeof(config.ssid));
  memcpy(config.password, stored.password, sizeof(config.password));
  memcpy(config.authCookie, stored.authCookie, sizeof(config.authCookie));
  memcpy(config.workspace, stored.workspace, sizeof(config.workspace));
  memcpy(config.queryId, stored.queryId, sizeof(config.queryId));
  config.ssid[sizeof(config.ssid) - 1] = '\0';
  config.password[sizeof(config.password) - 1] = '\0';
  config.authCookie[sizeof(config.authCookie) - 1] = '\0';
  config.workspace[sizeof(config.workspace) - 1] = '\0';
  config.queryId[sizeof(config.queryId) - 1] = '\0';
  config.pollIntervalSec = stored.pollIntervalSec;
}

} // namespace

void reset(WiFiConfig &config) {
  config = WiFiConfig{};
  config.pollIntervalSec = kDefaultPollIntervalSec;
  config.backlightTimeoutSec = kDefaultBacklightTimeoutSec;
}

bool validate(const WiFiConfig &config, char *error, size_t errorCapacity) {
  if (config.pollIntervalSec < 15 || config.pollIntervalSec > 86400) {
    setError(error, errorCapacity, "pollIntervalSec must be 15..86400");
    return false;
  }
  if (!backlight_timer::isSupportedTimeout(config.backlightTimeoutSec)) {
    setError(error, errorCapacity, "backlightTimeoutSec is unsupported");
    return false;
  }
  if (hasControlCharacter(config.ssid) ||
      hasControlCharacter(config.password) ||
      hasControlCharacter(config.authCookie) ||
      hasControlCharacter(config.workspace) ||
      hasControlCharacter(config.queryId)) {
    setError(error, errorCapacity, "config contains control characters");
    return false;
  }
  if (!config.enabled) {
    return true;
  }
  if (config.ssid[0] == '\0') {
    setError(error, errorCapacity, "ssid is required when WiFi is enabled");
    return false;
  }
  if (strncmp(config.authCookie, "auth=", 5) != 0 ||
      strlen(config.authCookie) <= 5 ||
      strchr(config.authCookie, ';') != nullptr) {
    setError(error, errorCapacity, "OpenCode authentication is required");
    return false;
  }
  if (strncmp(config.workspace, "wrk_", 4) != 0 ||
      strlen(config.workspace) < 14 || strlen(config.workspace) > 84 ||
      strspn(
          config.workspace + 4,
          "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") !=
          strlen(config.workspace + 4)) {
    setError(error, errorCapacity, "OpenCode workspace is invalid");
    return false;
  }
  if (config.queryId[0] != '\0' &&
      (strlen(config.queryId) != 64 ||
       strspn(config.queryId, "0123456789abcdef") != 64)) {
    setError(error, errorCapacity, "OpenCode query identifier is invalid");
    return false;
  }
  return true;
}

bool ConfigStore::begin() {
  if (started_) {
    return true;
  }
  started_ = preferences.begin("opencode", false);
  return started_;
}

bool ConfigStore::load(WiFiConfig &config) {
  reset(config);
  if (!begin()) {
    return false;
  }

  const size_t storedLength = preferences.getBytesLength(kConfigKey);
  if (storedLength == 0) {
    return false;
  }
  if (storedLength == sizeof(PersistedConfigV3)) {
    memset(&stored, 0, sizeof(stored));
    if (preferences.getBytes(kConfigKey, &stored.v3, sizeof(stored.v3)) !=
        sizeof(stored.v3)) {
      preferences.remove(kConfigKey);
      return false;
    }
    const uint32_t expectedChecksum =
        checksum(reinterpret_cast<const uint8_t *>(&stored.v3),
                 offsetof(PersistedConfigV3, checksum));
    if (stored.v3.schemaVersion != kConfigSchemaVersion ||
        stored.v3.checksum != expectedChecksum) {
      if (stored.v3.schemaVersion == kConfigSchemaVersion) {
        preferences.remove(kConfigKey);
      }
      return false;
    }
    copyCommonConfig(config, stored.v3);
    config.backlightTimeoutSec = stored.v3.backlightTimeoutSec;
  } else if (storedLength == sizeof(PersistedConfigV2)) {
    memset(&stored, 0, sizeof(stored));
    if (preferences.getBytes(kConfigKey, &stored.v2, sizeof(stored.v2)) !=
        sizeof(stored.v2)) {
      preferences.remove(kConfigKey);
      return false;
    }
    const uint32_t expectedChecksum =
        checksum(reinterpret_cast<const uint8_t *>(&stored.v2),
                 offsetof(PersistedConfigV2, checksum));
    if (stored.v2.schemaVersion != 2 ||
        stored.v2.checksum != expectedChecksum) {
      if (stored.v2.schemaVersion == 2) {
        preferences.remove(kConfigKey);
      }
      return false;
    }
    copyCommonConfig(config, stored.v2);
    config.backlightTimeoutSec = kDefaultBacklightTimeoutSec;
  } else {
    preferences.remove(kConfigKey);
    return false;
  }

  char error[96] = {};
  if (!validate(config, error, sizeof(error))) {
    reset(config);
    return false;
  }
  if (storedLength == sizeof(PersistedConfigV2)) {
    // Preserve a verified schema-2 record in RAM even if rewriting it fails;
    // the next boot will retry the schema-3 migration without losing auth.
    save(config, error, sizeof(error));
  }
  return config.enabled;
}

bool ConfigStore::save(const WiFiConfig &config, char *error,
                       size_t errorCapacity) {
  if (!validate(config, error, errorCapacity)) {
    return false;
  }
  if (!begin()) {
    setError(error, errorCapacity, "Preferences open failed");
    return false;
  }

  memset(&stored, 0, sizeof(stored));
  stored.v3.schemaVersion = kConfigSchemaVersion;
  stored.v3.enabled = config.enabled ? 1 : 0;
  memcpy(stored.v3.ssid, config.ssid, sizeof(stored.v3.ssid));
  memcpy(stored.v3.password, config.password, sizeof(stored.v3.password));
  memcpy(stored.v3.authCookie, config.authCookie, sizeof(stored.v3.authCookie));
  memcpy(stored.v3.workspace, config.workspace, sizeof(stored.v3.workspace));
  memcpy(stored.v3.queryId, config.queryId, sizeof(stored.v3.queryId));
  stored.v3.pollIntervalSec = config.pollIntervalSec;
  stored.v3.backlightTimeoutSec = config.backlightTimeoutSec;
  stored.v3.checksum = checksum(reinterpret_cast<const uint8_t *>(&stored.v3),
                                offsetof(PersistedConfigV3, checksum));

  const size_t written =
      preferences.putBytes(kConfigKey, &stored.v3, sizeof(stored.v3));
  if (written != sizeof(stored.v3)) {
    setError(error, errorCapacity, "Preferences write failed");
    return false;
  }
  return true;
}

} // namespace config_store
