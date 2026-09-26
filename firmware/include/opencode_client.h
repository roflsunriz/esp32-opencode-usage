#pragma once

#include <cstddef>
#include <cstdint>

#include "config_store.h"

#ifdef ARDUINO
#include <WiFiClientSecure.h>
#endif

namespace opencode_client {

constexpr size_t kUsagePayloadCapacity = 1024;

// These functions only read the known numerical fields from the official
// console status response. They never execute code received from the network.
bool normalizeConsoleStatusUsage(const char *response, size_t length,
                                 uint64_t updatedAtSec, char *output,
                                 size_t outputCapacity);

enum class Result : uint8_t {
  kOk,
  kAuthenticationRequired,
  kQueryRejected,
  kTransportFailed,
  kPayloadTooLarge,
  kResponseInvalid,
  kDiscoveryFailed,
};

// Redacted response metadata for USB diagnostics. Sizes and content-type
// flags only; never carries secrets, header values, or response bytes.
struct ResponseDiagnostic {
  int httpCode = 0;
  // HTTPClient getSize(); -1 when the server sent no Content-Length.
  int contentLength = -1;
  unsigned bodyBytes = 0;
  unsigned contentTypeLength = 0;
  bool isJson = false;
  bool isHtml = false;
};

// Distinct LCD status texts. kPayloadTooLarge previously shared the
// kResponseInvalid text, hiding whether the body was too big or malformed.
const char *responseStatusText(Result result);

// True when the server declared a length but fewer bytes arrived, e.g. a
// Wi-Fi drop mid-body. Chunked framing is validated by HTTPClient itself,
// so only positive declared lengths are checked here.
bool isTruncatedBody(int contentLength, size_t receivedBytes);

// Formats a ResponseDiagnostic as a single-line USB diagnostic frame.
// Returns the bytes written excluding the terminator, or 0 when too small.
size_t formatResponseDiagnostic(char *output, size_t capacity,
                                const char *result,
                                const ResponseDiagnostic &diagnostic);

struct TransportDiagnostic {
  int httpCode = 0;
  int tlsErrorCode = 0;
  char tlsError[96] = {};
};

#ifdef ARDUINO

namespace detail {
class BoundedStream;
}

class Client {
public:
  Client();

  bool getTransportDiagnostic(TransportDiagnostic &diagnostic) const;

  bool getResponseDiagnostic(ResponseDiagnostic &diagnostic) const;

  Result fetchUsage(const config_store::WiFiConfig &config,
                    uint64_t updatedAtSec, char *normalizedPayload,
                    size_t normalizedPayloadCapacity);

private:
  static constexpr size_t kUrlCapacity = 256;
  static constexpr size_t kPathCapacity = 256;
  static constexpr size_t kContentTypeCapacity = 64;

  Result getStatus(const config_store::WiFiConfig &config,
                   size_t maxResponseBytes);
  bool isJsonResponse() const;
  void clearTransportDiagnostic();
  void recordTransportFailure(int httpCode);
  void resetResponseMetadata();

  WiFiClientSecure tlsClient_;
  char requestUrl_[kUrlCapacity] = {};
  char requestPath_[kPathCapacity] = {};
  char contentType_[kContentTypeCapacity] = {};
  TransportDiagnostic transportDiagnostic_;
  bool hasTransportDiagnostic_ = false;
  int lastHttpCode_ = 0;
  int lastContentLength_ = -1;
  unsigned lastBodyBytes_ = 0;
};

#endif // ARDUINO

} // namespace opencode_client
