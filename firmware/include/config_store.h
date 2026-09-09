#pragma once

#include <cstddef>
#include <cstdint>

namespace config_store {

constexpr size_t kSsidCapacity = 64;
constexpr size_t kPasswordCapacity = 65;
constexpr size_t kAuthCookieCapacity = 4097;
constexpr size_t kWorkspaceCapacity = 96;
constexpr size_t kQueryIdCapacity = 65;
constexpr uint32_t kDefaultPollIntervalSec = 60;
constexpr uint32_t kDefaultBacklightTimeoutSec = 60;
constexpr uint32_t kConfigSchemaVersion = 3;

struct WiFiConfig {
  bool enabled = false;
  char ssid[kSsidCapacity] = {};
  char password[kPasswordCapacity] = {};
  char authCookie[kAuthCookieCapacity] = {};
  char workspace[kWorkspaceCapacity] = {};
  char queryId[kQueryIdCapacity] = {};
  uint32_t pollIntervalSec = kDefaultPollIntervalSec;
  uint32_t backlightTimeoutSec = kDefaultBacklightTimeoutSec;
};

void reset(WiFiConfig &config);
bool validate(const WiFiConfig &config, char *error, size_t errorCapacity);

class ConfigStore {
public:
  bool begin();
  bool load(WiFiConfig &config);
  bool save(const WiFiConfig &config, char *error, size_t errorCapacity);

private:
  bool started_ = false;
};

} // namespace config_store
