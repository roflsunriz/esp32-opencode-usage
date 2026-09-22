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
};

#endif // ARDUINO

} // namespace opencode_client
