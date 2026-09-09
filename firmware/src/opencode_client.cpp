#include "opencode_client.h"

#include <cstdio>
#include <cstring>

#include <HTTPClient.h>

#include "tls_roots.h"

namespace opencode_client {
namespace {

bool isWhitespace(char value) {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

bool isIdentifierCharacter(char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
         (value >= '0' && value <= '9') || value == '_' || value == '$';
}

bool isLowerHex(char value) {
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
}

bool validQueryId(const char *queryId) {
  if (queryId == nullptr)
    return false;
  for (size_t index = 0; index < kQueryIdLength; ++index) {
    if (!isLowerHex(queryId[index]))
      return false;
  }
  return queryId[kQueryIdLength] == '\0';
}

} // namespace

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
constexpr size_t kMaxDocumentBytes = 64 * 1024;
constexpr size_t kMaxUsageResponseBytes = 4 * 1024;
constexpr uint16_t kHttpTimeoutMs = 10000;
constexpr size_t kAssetPathCapacity = 160;
constexpr size_t kMaxIndexAssets = 8;

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

class AssetCollectorStream final : public detail::BoundedStream {
public:
  AssetCollectorStream(char (*items)[kAssetPathCapacity], size_t &count)
      : items_(items), count_(&count) {}

  bool valid() const override { return !invalid_; }

protected:
  void resetContent() override {
    memset(items_, 0, kMaxIndexAssets * kAssetPathCapacity);
    *count_ = 0;
    candidateLength_ = 0;
    prefixMatched_ = 0;
    suffixLength_ = 0;
    state_ = State::kPrefix;
    invalid_ = false;
  }

  void consume(const uint8_t *data, size_t length) override {
    for (size_t index = 0; index < length; ++index) {
      process(static_cast<char>(data[index]));
    }
  }

private:
  enum class State : uint8_t { kPrefix, kSuffix, kExpectJ, kExpectS };
  static const char *prefix() { return "/_build/assets/index-"; }
  static constexpr size_t kPrefixLength = sizeof("/_build/assets/index-") - 1;

  static bool isAssetPathCharacter(char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_' || value == '-';
  }

  void beginPrefix(char value) {
    const char *const assetPrefix = prefix();
    if (value == assetPrefix[prefixMatched_]) {
      ++prefixMatched_;
      if (prefixMatched_ == kPrefixLength) {
        memcpy(candidate_, assetPrefix, kPrefixLength);
        candidateLength_ = kPrefixLength;
        prefixMatched_ = 0;
        suffixLength_ = 0;
        state_ = State::kSuffix;
      }
      return;
    }
    prefixMatched_ = value == assetPrefix[0] ? 1 : 0;
  }

  void resetToPrefix(char value) {
    candidateLength_ = 0;
    suffixLength_ = 0;
    state_ = State::kPrefix;
    prefixMatched_ = 0;
    beginPrefix(value);
  }

  void addCandidate() {
    candidate_[candidateLength_] = '\0';
    for (size_t index = 0; index < *count_; ++index) {
      if (strcmp(items_[index], candidate_) == 0)
        return;
    }
    if (*count_ == kMaxIndexAssets) {
      invalid_ = true;
      return;
    }
    memcpy(items_[*count_], candidate_, candidateLength_ + 1);
    ++*count_;
  }

  void process(char value) {
    switch (state_) {
    case State::kPrefix:
      beginPrefix(value);
      return;
    case State::kSuffix:
      if (isAssetPathCharacter(value)) {
        if (candidateLength_ + 5 > kAssetPathCapacity) {
          invalid_ = true;
          resetToPrefix(value);
          return;
        }
        candidate_[candidateLength_++] = value;
        ++suffixLength_;
        return;
      }
      if (value == '.' && suffixLength_ > 0) {
        state_ = State::kExpectJ;
        return;
      }
      resetToPrefix(value);
      return;
    case State::kExpectJ:
      if (value == 'j') {
        state_ = State::kExpectS;
        return;
      }
      resetToPrefix(value);
      return;
    case State::kExpectS:
      if (value == 's') {
        candidate_[candidateLength_++] = '.';
        candidate_[candidateLength_++] = 'j';
        candidate_[candidateLength_++] = 's';
        addCandidate();
        resetToPrefix('\0');
        return;
      }
      resetToPrefix(value);
      return;
    }
  }

  char (*items_)[kAssetPathCapacity];
  size_t *count_ = nullptr;
  char candidate_[kAssetPathCapacity] = {};
  size_t candidateLength_ = 0;
  size_t prefixMatched_ = 0;
  size_t suffixLength_ = 0;
  State state_ = State::kPrefix;
  bool invalid_ = false;
};

class QueryIdStream final : public detail::BoundedStream {
public:
  const char *queryId() const { return queryId_; }
  bool hasQueryId() const { return hasQueryId_ && !ambiguous_; }
  bool valid() const override { return !ambiguous_; }

protected:
  void resetContent() override {
    state_ = State::kLookup;
    nameMatched_ = 0;
    hexDigits_ = 0;
    quote_ = '\0';
    previousIsIdentifier_ = false;
    hasQueryId_ = false;
    ambiguous_ = false;
    queryId_[0] = '\0';
    candidate_[0] = '\0';
  }

  void consume(const uint8_t *data, size_t length) override {
    for (size_t index = 0; index < length; ++index) {
      process(static_cast<char>(data[index]));
    }
  }

private:
  enum class State : uint8_t {
    kLookup,
    kAfterReference,
    kFunctionStart,
    kFunctionName,
    kAfterFunction,
    kAfterOpen,
    kHex,
    kAfterHex,
    kAfterQuote,
  };
  static const char *referenceName() { return "queryLiteSubscription_query"; }
  static constexpr size_t kReferenceNameLength =
      sizeof("queryLiteSubscription_query") - 1;

  void lookup(char value, bool previousIsIdentifier) {
    const char *const reference = referenceName();
    if (nameMatched_ == 0) {
      nameMatched_ = value == reference[0] && !previousIsIdentifier ? 1 : 0;
      return;
    }
    if (value == reference[nameMatched_]) {
      ++nameMatched_;
      if (nameMatched_ == kReferenceNameLength) {
        nameMatched_ = 0;
        state_ = State::kAfterReference;
      }
      return;
    }
    nameMatched_ = value == reference[0] && !previousIsIdentifier ? 1 : 0;
  }

  void restartLookup(char value, bool previousIsIdentifier) {
    state_ = State::kLookup;
    nameMatched_ = 0;
    lookup(value, previousIsIdentifier);
  }

  void foundQuery() {
    if (hasQueryId_) {
      ambiguous_ = true;
      queryId_[0] = '\0';
    } else {
      memcpy(queryId_, candidate_, kQueryIdLength);
      queryId_[kQueryIdLength] = '\0';
      hasQueryId_ = true;
    }
    state_ = State::kLookup;
    nameMatched_ = 0;
  }

  void process(char value) {
    const bool previousIsIdentifier = previousIsIdentifier_;
    switch (state_) {
    case State::kLookup:
      lookup(value, previousIsIdentifier);
      break;
    case State::kAfterReference:
      if (isWhitespace(value))
        break;
      if (value == '=') {
        state_ = State::kFunctionStart;
        break;
      }
      restartLookup(value, previousIsIdentifier);
      break;
    case State::kFunctionStart:
      if (isWhitespace(value))
        break;
      if (isIdentifierCharacter(value)) {
        state_ = State::kFunctionName;
        break;
      }
      restartLookup(value, previousIsIdentifier);
      break;
    case State::kFunctionName:
      if (isIdentifierCharacter(value))
        break;
      if (isWhitespace(value)) {
        state_ = State::kAfterFunction;
        break;
      }
      if (value == '(') {
        state_ = State::kAfterOpen;
        break;
      }
      restartLookup(value, previousIsIdentifier);
      break;
    case State::kAfterFunction:
      if (isWhitespace(value))
        break;
      if (value == '(') {
        state_ = State::kAfterOpen;
        break;
      }
      restartLookup(value, previousIsIdentifier);
      break;
    case State::kAfterOpen:
      if (isWhitespace(value))
        break;
      if (value == '\'' || value == '"') {
        quote_ = value;
        hexDigits_ = 0;
        state_ = State::kHex;
        break;
      }
      restartLookup(value, previousIsIdentifier);
      break;
    case State::kHex:
      if (isLowerHex(value) && hexDigits_ < kQueryIdLength) {
        candidate_[hexDigits_++] = value;
        if (hexDigits_ == kQueryIdLength)
          state_ = State::kAfterHex;
        break;
      }
      restartLookup(value, previousIsIdentifier);
      break;
    case State::kAfterHex:
      if (value == quote_) {
        state_ = State::kAfterQuote;
        break;
      }
      restartLookup(value, previousIsIdentifier);
      break;
    case State::kAfterQuote:
      if (isWhitespace(value))
        break;
      if (value == ')') {
        foundQuery();
        break;
      }
      restartLookup(value, previousIsIdentifier);
      break;
    }
    previousIsIdentifier_ = isIdentifierCharacter(value);
  }

  State state_ = State::kLookup;
  size_t nameMatched_ = 0;
  size_t hexDigits_ = 0;
  char quote_ = '\0';
  bool previousIsIdentifier_ = false;
  bool hasQueryId_ = false;
  bool ambiguous_ = false;
  char candidate_[kQueryIdLength + 1] = {};
  char queryId_[kQueryIdLength + 1] = {};
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

void Client::setCachedQueryId(const char *queryId) {
  clearCachedQueryId();
  if (validQueryId(queryId))
    memcpy(queryId_, queryId, kQueryIdLength + 1);
}

void Client::clearCachedQueryId() { memset(queryId_, 0, sizeof(queryId_)); }

bool Client::getTransportDiagnostic(TransportDiagnostic &diagnostic) const {
  if (!hasTransportDiagnostic_)
    return false;
  diagnostic = transportDiagnostic_;
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

bool Client::isJavaScriptResponse() const {
  return startsWithIgnoreCase(contentType_, "text/javascript");
}

Result Client::get(const config_store::WiFiConfig &config, const char *path,
                   detail::BoundedStream &sink, size_t maxResponseBytes) {
  clearTransportDiagnostic();
  resetResponseMetadata();
  if (path == nullptr || path[0] != '/' || maxResponseBytes == 0 ||
      maxResponseBytes > kMaxDocumentBytes ||
      snprintf(requestUrl_, sizeof(requestUrl_), "%s%s", kOpenCodeOrigin,
               path) >= static_cast<int>(sizeof(requestUrl_))) {
    return Result::kDiscoveryFailed;
  }
  tlsClient_.stop();
  HTTPClient http;
  http.setTimeout(kHttpTimeoutMs);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  const char *headers[] = {"Content-Type", "X-Error"};
  http.collectHeaders(headers, sizeof(headers) / sizeof(headers[0]));
  if (!http.begin(tlsClient_, requestUrl_)) {
    recordTransportFailure(0);
    return Result::kTransportFailed;
  }
  http.addHeader("Cookie", config.authCookie);
  const int statusCode = http.GET();
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
  if (statusCode != HTTP_CODE_OK || !http.header("X-Error").isEmpty()) {
    http.end();
    tlsClient_.stop();
    return Result::kDiscoveryFailed;
  }
  const String receivedContentType = http.header("Content-Type");
  snprintf(contentType_, sizeof(contentType_), "%s",
           receivedContentType.c_str());
  const int contentLength = http.getSize();
  if (contentLength > static_cast<int>(maxResponseBytes)) {
    http.end();
    tlsClient_.stop();
    return Result::kPayloadTooLarge;
  }
  sink.reset(maxResponseBytes);
  const int bodyResult = http.writeToStream(&sink);
  if (bodyResult < 0)
    recordTransportFailure(bodyResult);
  http.end();
  tlsClient_.stop();
  if (sink.overflowed())
    return Result::kPayloadTooLarge;
  if (bodyResult < 0)
    return Result::kTransportFailed;
  return sink.size() == 0 || !sink.valid() ? Result::kDiscoveryFailed
                                           : Result::kOk;
}

Result Client::postUsage(const config_store::WiFiConfig &config,
                         size_t maxResponseBytes) {
  clearTransportDiagnostic();
  resetResponseMetadata();
  if (!validQueryId(queryId_) || maxResponseBytes == 0 ||
      maxResponseBytes > kMaxUsageResponseBytes ||
      snprintf(requestUrl_, sizeof(requestUrl_), "%s/_server",
               kOpenCodeOrigin) >= static_cast<int>(sizeof(requestUrl_))) {
    return Result::kQueryRejected;
  }
  const int requestLength =
      snprintf(requestBody_, sizeof(requestBody_),
               "{\"t\":{\"t\":9,\"i\":0,\"l\":1,\"a\":[{\"t\":1,\"s\":\"%s\"}],"
               "\"o\":0},\"f\":31,\"m\":[]}",
               config.workspace);
  if (requestLength < 0 ||
      static_cast<size_t>(requestLength) >= sizeof(requestBody_)) {
    return Result::kResponseInvalid;
  }
  tlsClient_.stop();
  HTTPClient http;
  http.setTimeout(kHttpTimeoutMs);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  const char *headers[] = {"Content-Type", "X-Error"};
  http.collectHeaders(headers, sizeof(headers) / sizeof(headers[0]));
  if (!http.begin(tlsClient_, requestUrl_)) {
    recordTransportFailure(0);
    return Result::kTransportFailed;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Cookie", config.authCookie);
  http.addHeader("X-Server-Id", queryId_);
  http.addHeader("X-Server-Instance", "server-fn:0");
  const int statusCode = http.POST(reinterpret_cast<uint8_t *>(requestBody_),
                                   static_cast<size_t>(requestLength));
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
  if (!http.header("X-Error").isEmpty()) {
    http.end();
    tlsClient_.stop();
    return Result::kResponseInvalid;
  }
  const String receivedContentType = http.header("Content-Type");
  snprintf(contentType_, sizeof(contentType_), "%s",
           receivedContentType.c_str());
  const int contentLength = http.getSize();
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
  if (usageResponse.overflowed())
    return Result::kPayloadTooLarge;
  if (bodyResult < 0)
    return Result::kTransportFailed;
  return usageResponse.size() == 0 || !usageResponse.valid()
             ? Result::kResponseInvalid
             : Result::kOk;
}

Result Client::discoverQueryId(const config_store::WiFiConfig &config) {
  clearCachedQueryId();
  const int pagePathLength = snprintf(requestBody_, sizeof(requestBody_),
                                      "/workspace/%s/go", config.workspace);
  if (pagePathLength < 0 ||
      static_cast<size_t>(pagePathLength) >= sizeof(requestBody_)) {
    return Result::kDiscoveryFailed;
  }
  AssetCollectorStream assets(assetPaths_, assetCount_);
  Result result = get(config, requestBody_, assets, kMaxDocumentBytes);
  if (result != Result::kOk)
    return result;
  if (assetCount_ == 0)
    return Result::kDiscoveryFailed;
  for (size_t index = 0; index < assetCount_; ++index) {
    QueryIdStream query;
    result = get(config, assetPaths_[index], query, kMaxDocumentBytes);
    if (result == Result::kAuthenticationRequired ||
        result == Result::kTransportFailed ||
        result == Result::kPayloadTooLarge) {
      return result;
    }
    if (result == Result::kOk && query.hasQueryId()) {
      setCachedQueryId(query.queryId());
      return Result::kOk;
    }
  }
  return Result::kDiscoveryFailed;
}

Result Client::fetchUsage(const config_store::WiFiConfig &config,
                          uint64_t updatedAt, char *normalizedPayload,
                          size_t normalizedPayloadCapacity) {
  if (normalizedPayload == nullptr || normalizedPayloadCapacity == 0) {
    return Result::kResponseInvalid;
  }
  normalizedPayload[0] = '\0';
  if (!validQueryId(queryId_)) {
    const Result discovered = discoverQueryId(config);
    if (discovered != Result::kOk)
      return discovered;
  }
  for (uint8_t attempt = 0; attempt < 2; ++attempt) {
    const Result result = postUsage(config, kMaxUsageResponseBytes);
    if (result == Result::kOk) {
      if (!isJavaScriptResponse()) {
        return startsWithIgnoreCase(contentType_, "text/html")
                   ? Result::kAuthenticationRequired
                   : Result::kResponseInvalid;
      }
      return normalizeSerovalUsage(usageResponse.data(), usageResponse.size(),
                                   updatedAt, normalizedPayload,
                                   normalizedPayloadCapacity)
                 ? Result::kOk
                 : Result::kResponseInvalid;
    }
    if (result != Result::kQueryRejected || attempt == 1)
      return result;
    clearCachedQueryId();
    const Result discovered = discoverQueryId(config);
    if (discovered != Result::kOk)
      return discovered;
  }
  return Result::kQueryRejected;
}

} // namespace opencode_client
