#include "network_client.h"

#include <WiFi.h>
#include <esp_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstring>
#include <ctime>

namespace {

constexpr uint32_t kReconnectIntervalMs = 10000;
constexpr uint32_t kTimeSyncTimeoutMs = 20000;
constexpr uint32_t kTimeSyncRetryMs = 60000;
constexpr time_t kMinimumTrustedEpoch = 1735689600; // 2025-01-01 UTC

bool isDue(uint32_t nowMs, uint32_t targetMs) {
  return static_cast<int32_t>(nowMs - targetMs) >= 0;
}

bool hasSynchronizedTime() {
  const time_t now = time(nullptr);
  return now >= kMinimumTrustedEpoch &&
         sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED;
}

const char *responseResultFor(opencode_client::Result result) {
  switch (result) {
  case opencode_client::Result::kPayloadTooLarge:
    return "too_large";
  case opencode_client::Result::kResponseInvalid:
    return "invalid";
  default:
    return nullptr;
  }
}

void copyConfig(config_store::WiFiConfig &destination,
                const config_store::WiFiConfig &source) {
  // WiFiConfig contains the credential cookie. Assignment writes directly into
  // the persistent NetworkClient member and does not put a second 4 KiB copy
  // on the loop task stack.
  destination = source;
  destination.ssid[config_store::kSsidCapacity - 1] = '\0';
  destination.password[config_store::kPasswordCapacity - 1] = '\0';
  destination.authCookie[config_store::kAuthCookieCapacity - 1] = '\0';
  destination.workspace[config_store::kWorkspaceCapacity - 1] = '\0';
  destination.queryId[config_store::kQueryIdCapacity - 1] = '\0';
}

void writeJsonString(const char *value) {
  Serial.write('"');
  if (value != nullptr) {
    for (const char *cursor = value; *cursor != '\0'; ++cursor) {
      switch (*cursor) {
      case '"':
        Serial.print("\\\"");
        break;
      case '\\':
        Serial.print("\\\\");
        break;
      default: {
        const unsigned char character = static_cast<unsigned char>(*cursor);
        Serial.write(character >= 0x20 && character <= 0x7e ? character : '?');
        break;
      }
      }
    }
  }
  Serial.write('"');
}

void emitResponseDiagnostic(const char *result,
                            const opencode_client::Client &client) {
  // Redacted sizes only. Credentials, SSID, header values, and response
  // bytes must never be written to the serial log.
  opencode_client::ResponseDiagnostic diagnostic;
  client.getResponseDiagnostic(diagnostic);
  char frame[192] = {};
  if (opencode_client::formatResponseDiagnostic(frame, sizeof(frame), result,
                                                diagnostic) == 0) {
    return;
  }
  Serial.println(frame);
}

void emitTransportDiagnostic(
    const opencode_client::TransportDiagnostic &diagnostic) {
  // This intentionally emits only fixed diagnostics. Credentials, SSID, and
  // response bytes must never be written to the serial log.
  Serial.print(F("{\"version\":1,\"type\":\"diagnostic\",\"component\":"
                 "\"opencode_https\",\"httpCode\":"));
  Serial.print(diagnostic.httpCode);
  Serial.print(F(",\"tlsErrorCode\":"));
  Serial.print(diagnostic.tlsErrorCode);
  Serial.print(F(",\"tlsError\":"));
  writeJsonString(diagnostic.tlsError);
  Serial.print(F(",\"freeHeap\":"));
  Serial.print(ESP.getFreeHeap());
  Serial.print(F(",\"minFreeHeap\":"));
  Serial.print(ESP.getMinFreeHeap());
  Serial.print(F(",\"stackHighWaterMark\":"));
  Serial.print(uxTaskGetStackHighWaterMark(nullptr));
  Serial.println(F("}"));
}

} // namespace

void NetworkClient::setConfig(const config_store::WiFiConfig &config) {
  copyConfig(config_, config);
  configured_ = config_.enabled;
  memset(usagePayload_, 0, sizeof(usagePayload_));
  nextConnectAttemptMs_ = 0;
  nextTimeSyncAttemptMs_ = 0;
  timeSyncDeadlineMs_ = 0;
  nextPollMs_ = 0;
  timeSyncStarted_ = false;
  timeReady_ = false;
  lastStatus_[0] = '\0';

  // Keep the sole persistent copy of Wi-Fi credentials in ConfigStore. The
  // Arduino WiFi layer receives credentials only for this connection attempt.
  WiFi.persistent(false);
  if (!configured_) {
    WiFi.disconnect(false, false);
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
}

bool NetworkClient::isConnected() const {
  return WiFi.status() == WL_CONNECTED;
}

void NetworkClient::setPollIntervalSec(uint32_t pollIntervalSec) {
  config_.pollIntervalSec = pollIntervalSec;
}

uint32_t NetworkClient::pollRemainingMs(uint32_t nowMs) const {
  if (!configured_ || !timeReady_ || WiFi.status() != WL_CONNECTED) {
    return UINT32_MAX;
  }
  if (isDue(nowMs, nextPollMs_)) {
    return 0;
  }
  return nextPollMs_ - nowMs;
}

void NetworkClient::report(const char *status, StatusHandler statusHandler,
                           void *context) {
  if (status == nullptr || strcmp(lastStatus_, status) == 0) {
    return;
  }
  snprintf(lastStatus_, sizeof(lastStatus_), "%s", status);
  if (statusHandler != nullptr) {
    statusHandler(status, context);
  }
}

void NetworkClient::tick(uint32_t nowMs, PayloadHandler payloadHandler,
                         StatusHandler statusHandler, void *context) {
  if (!configured_) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    timeSyncStarted_ = false;
    timeReady_ = false;
    if (isDue(nowMs, nextConnectAttemptMs_)) {
      WiFi.persistent(false);
      WiFi.mode(WIFI_STA);
      WiFi.begin(config_.ssid, config_.password);
      nextConnectAttemptMs_ = nowMs + kReconnectIntervalMs;
      report("WiFi connecting", statusHandler, context);
    }
    return;
  }

  if (!timeReady_) {
    if (!timeSyncStarted_ && isDue(nowMs, nextTimeSyncAttemptMs_)) {
      // A verified TLS connection needs a real clock. There is deliberately no
      // insecure certificate fallback when NTP is unavailable.
      configTime(0, 0, "time.google.com", "time.cloudflare.com",
                 "pool.ntp.org");
      timeSyncStarted_ = true;
      timeSyncDeadlineMs_ = nowMs + kTimeSyncTimeoutMs;
      report("NTP syncing", statusHandler, context);
    }
    if (timeSyncStarted_ && hasSynchronizedTime()) {
      timeReady_ = true;
      timeSyncStarted_ = false;
      nextPollMs_ = nowMs;
      report("NTP synchronized", statusHandler, context);
    } else if (timeSyncStarted_ && isDue(nowMs, timeSyncDeadlineMs_)) {
      timeSyncStarted_ = false;
      nextTimeSyncAttemptMs_ = nowMs + kTimeSyncRetryMs;
      report("NTP sync failed", statusHandler, context);
    }
    return;
  }

  if (!isDue(nowMs, nextPollMs_)) {
    return;
  }
  const uint32_t intervalMs = config_.pollIntervalSec * 1000UL;
  nextPollMs_ = nowMs + intervalMs;
  report("OpenCode polling", statusHandler, context);

  const time_t wallClock = time(nullptr);
  if (wallClock < kMinimumTrustedEpoch) {
    timeReady_ = false;
    timeSyncStarted_ = false;
    nextTimeSyncAttemptMs_ = nowMs;
    report("NTP sync failed", statusHandler, context);
    return;
  }
  const opencode_client::Result result =
      opencode_.fetchUsage(config_, static_cast<uint64_t>(wallClock),
                           usagePayload_, sizeof(usagePayload_));
  if (result != opencode_client::Result::kOk) {
    if (result == opencode_client::Result::kTransportFailed) {
      opencode_client::TransportDiagnostic diagnostic;
      if (opencode_.getTransportDiagnostic(diagnostic)) {
        emitTransportDiagnostic(diagnostic);
      }
    }
    const char *responseResult = responseResultFor(result);
    if (responseResult != nullptr) {
      emitResponseDiagnostic(responseResult, opencode_);
    }
    report(opencode_client::responseStatusText(result), statusHandler,
           context);
    return;
  }

  const bool accepted =
      payloadHandler != nullptr && payloadHandler(usagePayload_, context);
  report(accepted ? "OpenCode updated" : "OpenCode payload rejected",
         statusHandler, context);
}
