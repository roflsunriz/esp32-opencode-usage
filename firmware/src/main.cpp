#include <Arduino.h>
#include <ArduinoJson.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "backlight_timer.h"
#include "config_store.h"
#include "display_controller.h"
#include "network_client.h"
#include "touch_ui.h"
#include "usage_model.h"

namespace {
constexpr size_t kSerialLineCapacity = 8192;
constexpr size_t kErrorCapacity = 128;
constexpr uint32_t kTouchNetworkDeferralMs = 150;

DisplayController display;
config_store::ConfigStore configStore;
config_store::WiFiConfig wifiConfig;
NetworkClient networkClient;
usage_model::UsageSnapshot latestUsage;
String serialLine;
bool serialLineOverflow = false;
bool hasUsage = false;
uint32_t renderCount = 0;
uint32_t lastFreshUsageMs = 0;
bool staleReported = false;
uint32_t deferNetworkUntilMs = 0;

void setErrorText(char *error, size_t capacity, const char *message) {
  if (error != nullptr && capacity > 0) {
    snprintf(error, capacity, "%s", message);
  }
}

bool readNumber(JsonVariantConst value, double &result) {
  if (value.isNull() || value.is<bool>() || value.is<const char *>()) {
    return false;
  }
  if (!value.as<JsonObjectConst>().isNull() ||
      !value.as<JsonArrayConst>().isNull()) {
    return false;
  }
  result = value.as<double>();
  return usage_model::isFinite(result);
}

bool readInteger(JsonVariantConst value, uint64_t &result) {
  double number = 0.0;
  if (!readNumber(value, number) || number < 0.0 ||
      std::floor(number) != number || number > 18446744073709549568.0) {
    return false;
  }
  result = static_cast<uint64_t>(number);
  return true;
}

bool readString(JsonVariantConst value, char *destination, size_t capacity,
                bool required) {
  if (value.isNull()) {
    if (capacity > 0) {
      destination[0] = '\0';
    }
    return !required;
  }
  if (!value.is<const char *>() || destination == nullptr || capacity == 0) {
    return false;
  }
  const char *source = value.as<const char *>();
  if (source == nullptr || strlen(source) >= capacity) {
    return false;
  }
  snprintf(destination, capacity, "%s", source);
  return true;
}

bool parsePeriod(JsonObjectConst root, const char *name,
                 usage_model::PeriodUsage &period, char *error,
                 size_t errorCapacity) {
  const JsonObjectConst object = root[name].as<JsonObjectConst>();
  if (object.isNull()) {
    snprintf(error, errorCapacity, "%s period is missing", name);
    return false;
  }

  double used = 0.0;
  double limit = 0.0;
  double percent = 0.0;
  if (!readNumber(object["used"], used) ||
      !readNumber(object["limit"], limit)) {
    snprintf(error, errorCapacity, "%s used/limit must be numbers", name);
    return false;
  }
  const bool hasPercent = !object["percent"].isNull();
  if (hasPercent && !readNumber(object["percent"], percent)) {
    snprintf(error, errorCapacity, "%s percent must be a number", name);
    return false;
  }

  int64_t resetInSec = -1;
  const bool hasReset = !object["resetInSec"].isNull();
  if (hasReset) {
    uint64_t resetValue = 0;
    if (!readInteger(object["resetInSec"], resetValue) ||
        resetValue > static_cast<uint64_t>(INT64_MAX)) {
      snprintf(error, errorCapacity, "%s resetInSec is invalid", name);
      return false;
    }
    resetInSec = static_cast<int64_t>(resetValue);
  }

  if (!usage_model::setPeriod(period, used, limit, percent, hasPercent,
                              resetInSec, hasReset)) {
    snprintf(error, errorCapacity, "%s values are invalid", name);
    return false;
  }
  return true;
}

bool parseUsage(JsonObjectConst root, usage_model::UsageSnapshot &snapshot,
                char *error, size_t errorCapacity) {
  uint64_t updatedAt = 0;
  if (!readInteger(root["updatedAt"], updatedAt)) {
    setErrorText(error, errorCapacity,
                 "updatedAt must be a non-negative integer");
    return false;
  }

  usage_model::UsageSnapshot parsed;
  parsed.updatedAt = updatedAt;
  if (!parsePeriod(root, "rolling", parsed.rolling, error, errorCapacity) ||
      !parsePeriod(root, "weekly", parsed.weekly, error, errorCapacity) ||
      !parsePeriod(root, "monthly", parsed.monthly, error, errorCapacity)) {
    return false;
  }
  parsed.valid = true;
  snapshot = parsed;
  return true;
}

bool parseConfig(JsonObjectConst root, config_store::WiFiConfig &config,
                 char *error, size_t errorCapacity) {
  config_store::WiFiConfig &parsed = config;
  config_store::reset(parsed);

  const JsonVariantConst enabledValue = root["enabled"];
  if (enabledValue.isNull()) {
    parsed.enabled = false;
  } else if (!enabledValue.is<bool>()) {
    setErrorText(error, errorCapacity, "enabled must be boolean");
    return false;
  } else {
    parsed.enabled = enabledValue.as<bool>();
  }
  if (!readString(root["ssid"], parsed.ssid, sizeof(parsed.ssid), false) ||
      !readString(root["password"], parsed.password, sizeof(parsed.password),
                  false) ||
      !readString(root["authCookie"], parsed.authCookie,
                  sizeof(parsed.authCookie), false) ||
      !readString(root["workspace"], parsed.workspace, sizeof(parsed.workspace),
                  false) ||
      !readString(root["queryId"], parsed.queryId, sizeof(parsed.queryId),
                  false)) {
    setErrorText(error, errorCapacity, "config text field is invalid");
    return false;
  }

  if (!root["enabled"].is<bool>()) {
    parsed.enabled = parsed.ssid[0] != '\0' && parsed.authCookie[0] != '\0';
  }

  if (!root["pollIntervalSec"].isNull()) {
    uint64_t interval = 0;
    if (!readInteger(root["pollIntervalSec"], interval) ||
        interval > UINT32_MAX) {
      setErrorText(error, errorCapacity, "pollIntervalSec is invalid");
      return false;
    }
    parsed.pollIntervalSec = static_cast<uint32_t>(interval);
  }

  if (!root["backlightTimeoutSec"].isNull()) {
    uint64_t timeout = 0;
    if (!readInteger(root["backlightTimeoutSec"], timeout) ||
        timeout > UINT32_MAX) {
      setErrorText(error, errorCapacity, "backlightTimeoutSec is invalid");
      return false;
    }
    parsed.backlightTimeoutSec = static_cast<uint32_t>(timeout);
  }

  if (!config_store::validate(parsed, error, errorCapacity)) {
    return false;
  }
  config = parsed;
  return true;
}

bool isDue(uint32_t nowMs, uint32_t targetMs) {
  return static_cast<int32_t>(nowMs - targetMs) >= 0;
}

bool readRequestId(JsonObjectConst root, const char *&requestId, char *error,
                   size_t errorCapacity, bool required = false) {
  requestId = nullptr;
  if (root["requestId"].isNull()) {
    if (!required) {
      return true;
    }
    setErrorText(error, errorCapacity, "requestId is required");
    return false;
  }
  if (!root["requestId"].is<const char *>()) {
    setErrorText(error, errorCapacity,
                 "requestId must be a hexadecimal string");
    return false;
  }
  requestId = root["requestId"].as<const char *>();
  if (requestId == nullptr || strlen(requestId) != 16 ||
      strspn(requestId, "0123456789abcdef") != 16) {
    setErrorText(error, errorCapacity,
                 "requestId must contain 16 hexadecimal characters");
    return false;
  }
  return true;
}

void addPeriod(JsonObject object, const usage_model::PeriodUsage &period) {
  object["used"] = period.used;
  object["limit"] = period.limit;
  object["percent"] = period.percent;
  if (period.resetInSec >= 0) {
    object["resetInSec"] = period.resetInSec;
  }
}

void addBacklightState(JsonDocument &document, bool includeRemaining) {
  document["screenFlipped"] = wifiConfig.screenFlipped;
  document["backlightOn"] = display.backlightOn();
  document["backlightTimeoutSec"] = display.backlightTimeoutSec();
  if (includeRemaining) {
    document["backlightRemainingMs"] = display.backlightRemainingMs();
  }
  document["backlightGpio"] = display.backlightGpioLevel();
}

void sendBacklightEvent() {
  JsonDocument document;
  document["version"] = usage_model::kProtocolVersion;
  document["type"] = "backlight";
  document["backlightOn"] = display.backlightOn();
  document["backlightTimeoutSec"] = display.backlightTimeoutSec();
  serializeJson(document, Serial);
  Serial.println();
}

void sendBacklightEventIfChanged() {
  if (display.consumeBacklightStateChanged()) {
    sendBacklightEvent();
  }
}

void sendError(const char *message) {
  JsonDocument document;
  document["version"] = usage_model::kProtocolVersion;
  document["type"] = "error";
  document["message"] = message == nullptr ? "unknown error" : message;
  serializeJson(document, Serial);
  Serial.println();
  display.showStatus(message == nullptr ? "Error" : message, ILI9341_RED);
}

void sendStatus(const char *message) {
  JsonDocument document;
  document["version"] = usage_model::kProtocolVersion;
  document["type"] = "status";
  document["message"] = message == nullptr ? "" : message;
  if (hasUsage) {
    document["renderCount"] = renderCount;
  }
  serializeJson(document, Serial);
  Serial.println();
}

void sendUsageAck(const usage_model::UsageSnapshot &snapshot,
                  const char *source) {
  JsonDocument document;
  document["version"] = usage_model::kProtocolVersion;
  document["type"] = "ack";
  document["accepted"] = "usage";
  document["source"] = source == nullptr ? "unknown" : source;
  document["updatedAt"] = snapshot.updatedAt;
  document["renderCount"] = renderCount;
  document["periodCount"] = usage_model::kPeriodCount;
  addPeriod(document["rolling"].to<JsonObject>(), snapshot.rolling);
  addPeriod(document["weekly"].to<JsonObject>(), snapshot.weekly);
  addPeriod(document["monthly"].to<JsonObject>(), snapshot.monthly);
  serializeJson(document, Serial);
  Serial.println();
}

void sendConfigAck(const char *requestId) {
  JsonDocument document;
  document["version"] = usage_model::kProtocolVersion;
  document["type"] = "ack";
  document["accepted"] = "config";
  if (requestId != nullptr)
    document["requestId"] = requestId;
  document["wifiEnabled"] = wifiConfig.enabled;
  document["pollIntervalSec"] = wifiConfig.pollIntervalSec;
  addBacklightState(document, false);
  document["freeHeap"] = ESP.getFreeHeap();
  document["minFreeHeap"] = ESP.getMinFreeHeap();
  serializeJson(document, Serial);
  Serial.println();
}

void sendDisplayAck(const char *requestId) {
  JsonDocument document;
  document["version"] = usage_model::kProtocolVersion;
  document["type"] = "ack";
  document["accepted"] = "display";
  if (requestId != nullptr) {
    document["requestId"] = requestId;
  }
  addBacklightState(document, false);
  serializeJson(document, Serial);
  Serial.println();
}

void sendWakeAck(const char *requestId) {
  JsonDocument document;
  document["version"] = usage_model::kProtocolVersion;
  document["type"] = "ack";
  document["accepted"] = "wake";
  if (requestId != nullptr) {
    document["requestId"] = requestId;
  }
  addBacklightState(document, false);
  serializeJson(document, Serial);
  Serial.println();
}

void sendPingAck() {
  JsonDocument document;
  document["version"] = usage_model::kProtocolVersion;
  document["type"] = "ack";
  document["accepted"] = "ping";
  document["freeHeap"] = ESP.getFreeHeap();
  document["minFreeHeap"] = ESP.getMinFreeHeap();
  document["firmware"] = "opencode-go-lcd";
  document["setupSchema"] = config_store::kSetupSchemaVersion;
  document["pollIntervalSec"] = wifiConfig.pollIntervalSec;
  document["uptimeMs"] = millis();
  document["hasUsage"] = hasUsage;
  document["renderCount"] = renderCount;
  document["wifiEnabled"] = wifiConfig.enabled;
  addBacklightState(document, true);
  serializeJson(document, Serial);
  Serial.println();
}

void sendReady() {
  JsonDocument document;
  document["version"] = usage_model::kProtocolVersion;
  document["type"] = "ready";
  document["firmware"] = "opencode-go-lcd";
  document["setupSchema"] = config_store::kSetupSchemaVersion;
  addBacklightState(document, true);
  serializeJson(document, Serial);
  Serial.println();
}

bool applyBacklightTimeout(uint32_t timeoutSec, char *error,
                           size_t errorCapacity) {
  if (!backlight_timer::isSupportedTimeout(timeoutSec)) {
    setErrorText(error, errorCapacity, "backlightTimeoutSec is unsupported");
    return false;
  }
  const uint32_t previousTimeout = wifiConfig.backlightTimeoutSec;
  wifiConfig.backlightTimeoutSec = timeoutSec;
  if (!configStore.save(wifiConfig, error, errorCapacity)) {
    wifiConfig.backlightTimeoutSec = previousTimeout;
    return false;
  }
  if (!display.setBacklightTimeoutSec(timeoutSec)) {
    wifiConfig.backlightTimeoutSec = previousTimeout;
    configStore.save(wifiConfig, error, errorCapacity);
    setErrorText(error, errorCapacity, "backlightTimeoutSec is unsupported");
    return false;
  }
  return true;
}

const char *touchEventName(TouchEventKind kind) {
  switch (kind) {
  case TouchEventKind::kWakeOnly:
    return "wake";
  case TouchEventKind::kTap:
    return "tap";
  case TouchEventKind::kUnreadable:
    return "unreadable";
  }
  return "unreadable";
}

const char *touchActionName(touch_ui::ActionKind kind) {
  switch (kind) {
  case touch_ui::ActionKind::kUsageTab:
    return "usage";
  case touch_ui::ActionKind::kDisplayTab:
    return "display";
  case touch_ui::ActionKind::kTimeout:
    return "timeout";
  case touch_ui::ActionKind::kNone:
    return "none";
  }
  return "none";
}

void sendTouchDiagnostic(const TouchEvent &event) {
  JsonDocument document;
  document["version"] = usage_model::kProtocolVersion;
  document["type"] = "touch";
  document["event"] = touchEventName(event.kind);
  if (event.hasCoordinates) {
    document["rawX"] = event.point.rawX;
    document["rawY"] = event.point.rawY;
    document["x"] = event.point.x;
    document["y"] = event.point.y;
    document["action"] = touchActionName(event.action.kind);
    if (event.action.kind == touch_ui::ActionKind::kTimeout) {
      document["backlightTimeoutSec"] = event.action.timeoutSec;
    }
  }
  serializeJson(document, Serial);
  Serial.println();
}

bool processTouch() {
  TouchEvent event;
  if (!display.pollTouch(event)) {
    return false;
  }
  sendTouchDiagnostic(event);
  if (event.kind != TouchEventKind::kTap) {
    return true;
  }

  switch (event.action.kind) {
  case touch_ui::ActionKind::kUsageTab:
    display.showUsageTab();
    if (hasUsage) {
      display.renderSnapshot(latestUsage);
    } else {
      display.renderNoData("Set up WiFi on your PC");
    }
    break;
  case touch_ui::ActionKind::kDisplayTab:
    display.showDisplayTab(wifiConfig.backlightTimeoutSec);
    break;
  case touch_ui::ActionKind::kTimeout: {
    char error[kErrorCapacity] = {};
    if (applyBacklightTimeout(event.action.timeoutSec, error, sizeof(error))) {
      display.showDisplayTab(wifiConfig.backlightTimeoutSec);
      display.showStatus("Screen timeout saved", ILI9341_GREEN);
      // There is no serial requestId for an on-device tap, but the same ACK
      // lets a connected setup UI synchronize the persisted selection.
      sendDisplayAck(nullptr);
    } else {
      display.showStatus(error, ILI9341_RED);
      sendError(error);
    }
    break;
  }
  case touch_ui::ActionKind::kNone:
    break;
  }
  return true;
}

bool processBootButton() {
  if (!display.consumeBootClick()) {
    return false;
  }
  display.wakeBacklight();
  wifiConfig.screenFlipped = !wifiConfig.screenFlipped;
  char error[kErrorCapacity] = {};
  if (!configStore.save(wifiConfig, error, sizeof(error))) {
    wifiConfig.screenFlipped = !wifiConfig.screenFlipped;
    sendError(error);
    return true;
  }
  display.setScreenFlipped(wifiConfig.screenFlipped);
  display.redraw(hasUsage ? &latestUsage : nullptr,
                 wifiConfig.backlightTimeoutSec);
  sendDisplayAck(nullptr);
  return true;
}

bool processFrame(const char *payload, const char *source) {
  if (payload == nullptr || payload[0] == '\0') {
    sendError("empty JSON frame");
    return false;
  }
  JsonDocument document;
  const DeserializationError deserialization =
      deserializeJson(document, payload);
  if (deserialization) {
    sendError("invalid JSON frame");
    return false;
  }
  const JsonObjectConst root = document.as<JsonObjectConst>();
  if (root.isNull()) {
    sendError("JSON frame must be an object");
    return false;
  }

  uint64_t version = 0;
  if (!readInteger(root["version"], version) ||
      version != usage_model::kProtocolVersion) {
    sendError("unsupported protocol version");
    return false;
  }
  const JsonVariantConst typeValue = root["type"];
  if (!typeValue.is<const char *>()) {
    sendError("type must be a string");
    return false;
  }
  const char *type = typeValue.as<const char *>();
  char error[kErrorCapacity] = {};

  if (strcmp(type, "usage") == 0) {
    usage_model::UsageSnapshot parsed;
    if (!parseUsage(root, parsed, error, sizeof(error))) {
      sendError(error);
      return false;
    }
    if (!hasUsage || parsed.updatedAt != latestUsage.updatedAt) {
      lastFreshUsageMs = millis();
      staleReported = false;
    }
    latestUsage = parsed;
    hasUsage = true;
    ++renderCount;
    display.renderSnapshot(latestUsage);
    if (staleReported)
      display.showStatus("STALE - check WiFi / login", ILI9341_ORANGE);
    sendUsageAck(latestUsage, source);
    return true;
  }

  if (strcmp(type, "error") == 0) {
    const JsonVariantConst message = root["message"];
    if (!message.is<const char *>()) {
      sendError("error message must be a string");
      return false;
    }
    display.showStatus(message.as<const char *>(), ILI9341_RED);
    sendStatus("Remote error");
    return true;
  }

  if (strcmp(type, "config") == 0) {
    const char *requestId = nullptr;
    if (!readRequestId(root, requestId, error, sizeof(error))) {
      sendError(error);
      return false;
    }
    static config_store::WiFiConfig parsed;
    if (!parseConfig(root, parsed, error, sizeof(error))) {
      sendError(error);
      return false;
    }
    // The PC setup protocol owns Wi-Fi/auth/timeout, not the BOOT preference.
    parsed.screenFlipped = wifiConfig.screenFlipped;
    if (!configStore.save(parsed, error, sizeof(error))) {
      sendError(error);
      return false;
    }
    wifiConfig = parsed;
    networkClient.setConfig(wifiConfig);
    display.setBacklightTimeoutSec(wifiConfig.backlightTimeoutSec);
    if (display.activeTab() == touch_ui::Tab::kDisplay) {
      display.showDisplayTab(wifiConfig.backlightTimeoutSec);
    }
    display.showStatus(wifiConfig.enabled ? "WiFi config saved"
                                          : "Setup cleared - use PC",
                       ILI9341_GREEN);
    sendConfigAck(requestId);
    return true;
  }

  if (strcmp(type, "display") == 0) {
    const char *requestId = nullptr;
    if (!readRequestId(root, requestId, error, sizeof(error), true)) {
      sendError(error);
      return false;
    }
    uint64_t timeout = 0;
    if (!readInteger(root["backlightTimeoutSec"], timeout) ||
        timeout > UINT32_MAX ||
        !backlight_timer::isSupportedTimeout(static_cast<uint32_t>(timeout))) {
      sendError("backlightTimeoutSec is unsupported");
      return false;
    }
    if (!applyBacklightTimeout(static_cast<uint32_t>(timeout), error,
                               sizeof(error))) {
      sendError(error);
      return false;
    }
    if (display.activeTab() == touch_ui::Tab::kDisplay) {
      display.showDisplayTab(wifiConfig.backlightTimeoutSec);
    }
    sendDisplayAck(requestId);
    return true;
  }

  if (strcmp(type, "wake") == 0) {
    const char *requestId = nullptr;
    if (!readRequestId(root, requestId, error, sizeof(error))) {
      sendError(error);
      return false;
    }
    display.wakeBacklight();
    sendWakeAck(requestId);
    return true;
  }

  if (strcmp(type, "screenshot") == 0 && strcmp(source, "serial") == 0) {
    display.capture();
    return true;
  }

  if (strcmp(type, "ping") == 0) {
    sendPingAck();
    return true;
  }

  sendError("unsupported frame type");
  return false;
}

void processSerialLine() {
  if (serialLineOverflow) {
    sendError("serial frame exceeds 8191 bytes");
  } else if (!serialLine.isEmpty()) {
    processFrame(serialLine.c_str(), "serial");
  }
  serialLine = "";
  serialLineOverflow = false;
}

void pollSerial() {
  while (Serial.available() > 0) {
    const char character = static_cast<char>(Serial.read());
    if (character == '\n') {
      processSerialLine();
      continue;
    }
    if (character == '\r') {
      continue;
    }
    if (serialLine.length() >= kSerialLineCapacity - 1) {
      serialLineOverflow = true;
      continue;
    }
    serialLine += character;
  }
}

void onNetworkStatus(const char *status, void * /*context*/) {
  const bool failed =
      status != nullptr && (strstr(status, "failed") != nullptr ||
                            strstr(status, "rejected") != nullptr ||
                            strstr(status, "status ") != nullptr ||
                            strstr(status, "invalid") != nullptr);
  display.showStatus(status == nullptr ? "Network" : status,
                     failed ? ILI9341_RED : ILI9341_YELLOW);
  sendStatus(status);
}

bool onNetworkPayload(const char *payload, void * /*context*/) {
  return processFrame(payload, "http");
}

} // namespace

void setup() {
  Serial.begin(115200);
  serialLine.reserve(kSerialLineCapacity);
  delay(50);

  config_store::reset(wifiConfig);
  configStore.begin();
  const bool wifiConfigured = configStore.load(wifiConfig);
  display.begin(wifiConfig.screenFlipped);
  display.renderNoData("Set up WiFi on your PC");
  // load() intentionally returns false for a valid disabled Wi-Fi record.
  // Its display timeout is still persisted and must survive that restart.
  display.setBacklightTimeoutSec(wifiConfig.backlightTimeoutSec);
  if (wifiConfigured) {
    networkClient.setConfig(wifiConfig);
    display.showStatus("WiFi config loaded", ILI9341_YELLOW);
  } else {
    display.showStatus("Set up WiFi on your PC", ILI9341_YELLOW);
  }
  sendReady();
}

void loop() {
  pollSerial();
  const bool bootHandled = processBootButton();
  const bool touchHandled = processTouch();
  if (touchHandled || bootHandled) {
    deferNetworkUntilMs = millis() + kTouchNetworkDeferralMs;
  }
  sendBacklightEventIfChanged();
  const uint32_t nowMs = millis();
  if (!display.hasTouchPending() && isDue(nowMs, deferNetworkUntilMs)) {
    networkClient.tick(nowMs, onNetworkPayload, onNetworkStatus, nullptr);
  }
  sendBacklightEventIfChanged();
  const uint32_t pollInterval =
      wifiConfig.enabled ? wifiConfig.pollIntervalSec : 60;
  if (hasUsage && !staleReported &&
      usage_model::isStale(millis() - lastFreshUsageMs, pollInterval)) {
    staleReported = true;
    display.showStatus("STALE - check WiFi / login", ILI9341_ORANGE);
    sendStatus("Usage is stale");
  }
  delay(2);
}
