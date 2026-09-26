#include "opencode_client.h"

#include <cstdio>
#include <cstring>

#include <HTTPClient.h>

#include "tls_roots.h"

namespace opencode_client {

namespace detail {

class BoundedStream : public Stream {
public:
  void reset(size_t maximum) {
    total_ = 0;
    maximum_ = maximum;
    overflowed_ = false;
    resetContent();
  }

  size_t write(const uint8_t *data, size_t length) override {
    const size_t remaining = maximum_ > total_ ? maximum_ - total_ : 0;
    const size_t accepted = length < remaining ? length : remaining;
    if (accepted > 0) {
      consume(data, accepted);
      total_ += accepted;
    }
    if (accepted != length)
      overflowed_ = true;
    return accepted;
  }

  size_t write(uint8_t value) override { return write(&value, 1); }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  size_t size() const { return total_; }
  bool overflowed() const { return overflowed_; }
  virtual bool valid() const = 0;

protected:
  virtual void resetContent() = 0;
  virtual void consume(const uint8_t *data, size_t length) = 0;

private:
  size_t total_ = 0;
  size_t maximum_ = 0;
  bool overflowed_ = false;
};

} // namespace detail

namespace {

constexpr char kOpenCodeOrigin[] = "https://opencode.ai";
constexpr char kStatusPath[] = "/console/api/go/status";
constexpr size_t kMaxUsageResponseBytes = 4 * 1024;
constexpr uint16_t kHttpTimeoutMs = 10000;

class UsageBodyStream final : public detail::BoundedStream {
public:
  const char *data() const { return reinterpret_cast<const char *>(buffer_); }
  bool valid() const override { return true; }

protected:
  void resetContent() override { buffer_[0] = '\0'; }
  void consume(const uint8_t *data, size_t length) override {
    memcpy(buffer_ + size(), data, length);
    buffer_[size() + length] = '\0';
  }

private:
  uint8_t buffer_[kMaxUsageResponseBytes + 1] = {};
};

UsageBodyStream usageResponse;

bool isAuthenticationStatus(int statusCode) {
  return (statusCode >= 300 && statusCode < 400) || statusCode == 401 ||
         statusCode == 403;
}

char lowerAscii(char value) {
  return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a')
                                      : value;
}

bool startsWithIgnoreCase(const char *value, const char *prefix) {
  if (value == nullptr || prefix == nullptr)
    return false;
  for (size_t index = 0; prefix[index] != '\0'; ++index) {
    if (value[index] == '\0' || lowerAscii(value[index]) != prefix[index]) {
      return false;
    }
  }
  return true;
}

} // namespace

Client::Client() {
  tlsClient_.setCACert(tls_roots::kOpenCodeTrustAnchors);
  tlsClient_.setHandshakeTimeout(10);
}

bool Client::getTransportDiagnostic(TransportDiagnostic &diagnostic) const {
  if (!hasTransportDiagnostic_)
    return false;
  diagnostic = transportDiagnostic_;
  return true;
}

bool Client::getResponseDiagnostic(
    ResponseDiagnostic &diagnostic) const {
  diagnostic.httpCode = lastHttpCode_;
  diagnostic.contentLength = lastContentLength_;
  diagnostic.bodyBytes = lastBodyBytes_;
  diagnostic.contentTypeLength =
      static_cast<unsigned>(strlen(contentType_));
  diagnostic.isJson = isJsonResponse();
  diagnostic.isHtml = startsWithIgnoreCase(contentType_, "text/html");
  return true;
}

void Client::clearTransportDiagnostic() {
  memset(&transportDiagnostic_, 0, sizeof(transportDiagnostic_));
  hasTransportDiagnostic_ = false;
}

void Client::recordTransportFailure(int httpCode) {
  transportDiagnostic_.httpCode = httpCode;
  transportDiagnostic_.tlsErrorCode = tlsClient_.lastError(
      transportDiagnostic_.tlsError, sizeof(transportDiagnostic_.tlsError));
  for (char *cursor = transportDiagnostic_.tlsError; *cursor != '\0';
       ++cursor) {
    const unsigned char value = static_cast<unsigned char>(*cursor);
    if (value < 0x20 || value == 0x7f)
      *cursor = ' ';
  }
  hasTransportDiagnostic_ = true;
}

void Client::resetResponseMetadata() { contentType_[0] = '\0'; }

bool Client::isJsonResponse() const {
  return startsWithIgnoreCase(contentType_, "application/json");
}

Result Client::getStatus(const config_store::WiFiConfig &config,
                         size_t maxResponseBytes) {
  clearTransportDiagnostic();
  resetResponseMetadata();
  lastHttpCode_ = 0;
  lastContentLength_ = -1;
  lastBodyBytes_ = 0;
  if (config.authCookie[0] == '\0' || config.workspace[0] == '\0' ||
      maxResponseBytes == 0 || maxResponseBytes > kMaxUsageResponseBytes ||
      snprintf(requestUrl_, sizeof(requestUrl_), "%s%s", kOpenCodeOrigin,
               kStatusPath) >= static_cast<int>(sizeof(requestUrl_)) ||
      snprintf(requestPath_, sizeof(requestPath_), "%s", kStatusPath) < 0) {
    return Result::kResponseInvalid;
  }
  tlsClient_.stop();
  HTTPClient http;
  http.setTimeout(kHttpTimeoutMs);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  const char *headers[] = {"Content-Type"};
  http.collectHeaders(headers, sizeof(headers) / sizeof(headers[0]));
  if (!http.begin(tlsClient_, requestUrl_)) {
    recordTransportFailure(0);
    return Result::kTransportFailed;
  }
  http.addHeader("Cookie", config.authCookie);
  http.addHeader("x-org-id", config.workspace);
  const int statusCode = http.GET();
  lastHttpCode_ = statusCode;
  if (statusCode <= 0) {
    recordTransportFailure(statusCode);
    http.end();
    tlsClient_.stop();
    return Result::kTransportFailed;
  }
  if (isAuthenticationStatus(statusCode)) {
    http.end();
    tlsClient_.stop();
    return Result::kAuthenticationRequired;
  }
  if (statusCode != HTTP_CODE_OK) {
    http.end();
    tlsClient_.stop();
    return Result::kQueryRejected;
  }
  const String receivedContentType = http.header("Content-Type");
  snprintf(contentType_, sizeof(contentType_), "%s",
           receivedContentType.c_str());
  const int contentLength = http.getSize();
  lastContentLength_ = contentLength;
  if (contentLength > static_cast<int>(maxResponseBytes)) {
    http.end();
    tlsClient_.stop();
    return Result::kPayloadTooLarge;
  }
  usageResponse.reset(maxResponseBytes);
  const int bodyResult = http.writeToStream(&usageResponse);
  if (bodyResult < 0)
    recordTransportFailure(bodyResult);
  http.end();
  tlsClient_.stop();
  lastBodyBytes_ = static_cast<unsigned>(usageResponse.size());
  if (usageResponse.overflowed())
    return Result::kPayloadTooLarge;
  if (bodyResult < 0)
    return Result::kTransportFailed;
  if (isTruncatedBody(contentLength, usageResponse.size())) {
    recordTransportFailure(statusCode);
    return Result::kTransportFailed;
  }
  return usageResponse.size() == 0 || !usageResponse.valid()
             ? Result::kResponseInvalid
             : Result::kOk;
}

Result Client::fetchUsage(const config_store::WiFiConfig &config,
                          uint64_t updatedAtSec, char *normalizedPayload,
                          size_t normalizedPayloadCapacity) {
  if (normalizedPayload == nullptr || normalizedPayloadCapacity == 0) {
    return Result::kResponseInvalid;
  }
  normalizedPayload[0] = '\0';
  const Result result = getStatus(config, kMaxUsageResponseBytes);
  if (result != Result::kOk)
    return result;
  if (!isJsonResponse()) {
    return startsWithIgnoreCase(contentType_, "text/html")
               ? Result::kAuthenticationRequired
               : Result::kResponseInvalid;
  }
  return normalizeConsoleStatusUsage(usageResponse.data(), usageResponse.size(),
                                     updatedAtSec, normalizedPayload,
                                     normalizedPayloadCapacity)
             ? Result::kOk
             : Result::kResponseInvalid;
}

} // namespace opencode_client
