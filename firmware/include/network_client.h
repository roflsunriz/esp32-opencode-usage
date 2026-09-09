#pragma once

#include <cstddef>
#include <cstdint>

#include "config_store.h"
#include "opencode_client.h"

class NetworkClient {
public:
  using PayloadHandler = bool (*)(const char *payload, void *context);
  using StatusHandler = void (*)(const char *status, void *context);

  void setConfig(const config_store::WiFiConfig &config);
  bool isConfigured() const { return configured_; }
  bool isConnected() const;
  void tick(uint32_t nowMs, PayloadHandler payloadHandler,
            StatusHandler statusHandler, void *context);

private:
  void report(const char *status, StatusHandler statusHandler, void *context);

  config_store::WiFiConfig config_;
  opencode_client::Client opencode_;
  char usagePayload_[opencode_client::kUsagePayloadCapacity] = {};
  bool configured_ = false;
  bool timeSyncStarted_ = false;
  bool timeReady_ = false;
  uint32_t nextConnectAttemptMs_ = 0;
  uint32_t nextTimeSyncAttemptMs_ = 0;
  uint32_t timeSyncDeadlineMs_ = 0;
  uint32_t nextPollMs_ = 0;
  char lastStatus_[64] = {};
};
