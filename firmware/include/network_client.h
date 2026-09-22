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
  // Updates only the poll cadence without dropping the Wi-Fi connection.
  // Used by the on-device poll-interval slider.
  void setPollIntervalSec(uint32_t pollIntervalSec);
  bool isConfigured() const { return configured_; }
  bool isConnected() const;
  // Milliseconds until the next scheduled poll, or UINT32_MAX when the
  // cadence is unknown (unconfigured, offline, or time not synchronized).
  // Returns 0 when a poll is due or in flight.
  uint32_t pollRemainingMs(uint32_t nowMs) const;
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
