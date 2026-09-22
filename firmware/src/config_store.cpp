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
constexpr uint32_t kPersistedConfigSchemaV2 = 2;
constexpr uint32_t kPersistedConfigSchemaV3 = 3;

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

struct PersistedConfigV4 {
  uint32_t schemaVersion;
  uint8_t enabled;
  // V4 assigns reserved[0] to screenFlipped. The remaining bytes must stay
  // zero so malformed records are rejected instead of silently accepted.
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
  PersistedConfigV4 v4;
};

PersistedStorage stored;

static_assert(offsetof(PersistedConfigV2, checksum) + sizeof(uint32_t) ==
                  sizeof(PersistedConfigV2),
              "PersistedConfigV2 must have checksum as its final field");
static_assert(offsetof(PersistedConfigV3, checksum) + sizeof(uint32_t) ==
                  sizeof(PersistedConfigV3),
              "PersistedConfigV3 must have checksum as its final field");
static_assert(offsetof(PersistedConfigV4, checksum) + sizeof(uint32_t) ==
                  sizeof(PersistedConfigV4),
              "PersistedConfigV4 must have checksum as its final field");
static_assert(sizeof(PersistedConfigV3) == sizeof(PersistedConfigV4),
              "PersistedConfigV4 must preserve the V3 record size");

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

bool hasNulTerminator(const char *value, size_t capacity) {
  for (size_t index = 0; index < capacity; ++index) {
    if (value[index] == '\0') {
      return true;
    }
  }
  return false;
}

template <typename Persisted>
bool persistedStringsAreTerminated(const Persisted &value) {
  return hasNulTerminator(value.ssid, sizeof(value.ssid)) &&
         hasNulTerminator(value.password, sizeof(value.password)) &&
         hasNulTerminator(value.authCookie, sizeof(value.authCookie)) &&
         hasNulTerminator(value.workspace, sizeof(value.workspace)) &&
         hasNulTerminator(value.queryId, sizeof(value.queryId));
}

template <typename Persisted>
bool persistedCommonValuesAreValid(const Persisted &value) {
  return value.enabled <= 1 && persistedStringsAreTerminated(value);
}

bool persistedV4ValuesAreValid(const PersistedConfigV4 &value) {
  return persistedCommonValuesAreValid(value) && value.reserved[0] <= 1 &&
         value.reserved[1] == 0 && value.reserved[2] == 0;
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
  if (!hasNulTerminator(config.ssid, sizeof(config.ssid)) ||
      !hasNulTerminator(config.password, sizeof(config.password)) ||
      !hasNulTerminator(config.authCookie, sizeof(config.authCookie)) ||
      !hasNulTerminator(config.workspace, sizeof(config.workspace)) ||
      !hasNulTerminator(config.queryId, sizeof(config.queryId))) {
    setError(error, errorCapacity, "config string is not terminated");
    return false;
  }
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
  if (strncmp(config.authCookie, "__Host-console_session=", 23) != 0 ||
      strlen(config.authCookie) <= 23 ||
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
  bool recognizedRecord = false;
  bool migrateToCurrentSchema = false;
  if (storedLength == sizeof(PersistedConfigV4)) {
    memset(&stored, 0, sizeof(stored));
    if (preferences.getBytes(kConfigKey, &stored.v4, sizeof(stored.v4)) !=
        sizeof(stored.v4)) {
      preferences.remove(kConfigKey);
      return false;
    }
    const uint32_t persistedSchema = stored.v4.schemaVersion;
    if (persistedSchema == kConfigSchemaVersion) {
      recognizedRecord = true;
      const uint32_t expectedChecksum =
          checksum(reinterpret_cast<const uint8_t *>(&stored.v4),
                   offsetof(PersistedConfigV4, checksum));
      if (stored.v4.checksum != expectedChecksum ||
          !persistedV4ValuesAreValid(stored.v4)) {
        preferences.remove(kConfigKey);
        return false;
      }
      copyCommonConfig(config, stored.v4);
      config.backlightTimeoutSec = stored.v4.backlightTimeoutSec;
      config.screenFlipped = stored.v4.reserved[0] != 0;
    } else if (persistedSchema == kPersistedConfigSchemaV3) {
      recognizedRecord = true;
      const uint32_t expectedChecksum =
          checksum(reinterpret_cast<const uint8_t *>(&stored.v3),
                   offsetof(PersistedConfigV3, checksum));
      if (stored.v3.checksum != expectedChecksum ||
          !persistedCommonValuesAreValid(stored.v3)) {
        preferences.remove(kConfigKey);
        return false;
      }
      copyCommonConfig(config, stored.v3);
      config.backlightTimeoutSec = stored.v3.backlightTimeoutSec;
      config.screenFlipped = false;
      migrateToCurrentSchema = true;
    } else {
      // Keep records from a newer firmware intact so a later compatible
      // firmware can still read them.
      return false;
    }
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
    if (stored.v2.schemaVersion != kPersistedConfigSchemaV2 ||
        stored.v2.checksum != expectedChecksum ||
        !persistedCommonValuesAreValid(stored.v2)) {
      if (stored.v2.schemaVersion == kPersistedConfigSchemaV2) {
        preferences.remove(kConfigKey);
      }
      return false;
    }
    recognizedRecord = true;
    copyCommonConfig(config, stored.v2);
    config.backlightTimeoutSec = kDefaultBacklightTimeoutSec;
    config.screenFlipped = false;
    migrateToCurrentSchema = true;
  } else {
    preferences.remove(kConfigKey);
    return false;
  }

  char error[96] = {};
  if (!validate(config, error, sizeof(error))) {
    if (recognizedRecord) {
      preferences.remove(kConfigKey);
    }
    reset(config);
    return false;
  }
  if (migrateToCurrentSchema) {
    // Preserve a verified legacy record in RAM even if rewriting it fails;
    // the next boot will retry migration without losing authentication.
    (void)save(config, error, sizeof(error));
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
  stored.v4.schemaVersion = kConfigSchemaVersion;
  stored.v4.enabled = config.enabled ? 1 : 0;
  stored.v4.reserved[0] = config.screenFlipped ? 1 : 0;
  memcpy(stored.v4.ssid, config.ssid, sizeof(stored.v4.ssid));
  memcpy(stored.v4.password, config.password, sizeof(stored.v4.password));
  memcpy(stored.v4.authCookie, config.authCookie,
         sizeof(stored.v4.authCookie));
  memcpy(stored.v4.workspace, config.workspace, sizeof(stored.v4.workspace));
  memcpy(stored.v4.queryId, config.queryId, sizeof(stored.v4.queryId));
  stored.v4.pollIntervalSec = config.pollIntervalSec;
  stored.v4.backlightTimeoutSec = config.backlightTimeoutSec;
  stored.v4.checksum = checksum(reinterpret_cast<const uint8_t *>(&stored.v4),
                                offsetof(PersistedConfigV4, checksum));

  const size_t written =
      preferences.putBytes(kConfigKey, &stored.v4, sizeof(stored.v4));
  if (written != sizeof(stored.v4)) {
    setError(error, errorCapacity, "Preferences write failed");
    return false;
  }
  return true;
}

} // namespace config_store
