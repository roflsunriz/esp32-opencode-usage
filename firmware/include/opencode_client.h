#pragma once

#include <cstddef>
#include <cstdint>

#include "config_store.h"

#ifdef ARDUINO
#include <WiFiClientSecure.h>
#endif

namespace opencode_client {

constexpr size_t kQueryIdLength = 64;
constexpr size_t kUsagePayloadCapacity = 1024;

// These functions only read the known numerical fields from the official
// response. They never execute JavaScript received from the network.
bool extractQueryIdFromScript(const char *script, size_t length, char *queryId,
                              size_t queryIdCapacity);
bool normalizeSerovalUsage(const char *response, size_t length,
                           uint64_t updatedAt, char *output,
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

  // The initial value is supplied during USB setup. A deployment change is
  // handled by re-discovering a replacement in RAM when the server rejects it.
  void setCachedQueryId(const char *queryId);
  void clearCachedQueryId();
  bool getTransportDiagnostic(TransportDiagnostic &diagnostic) const;

  Result fetchUsage(const config_store::WiFiConfig &config, uint64_t updatedAt,
                    char *normalizedPayload, size_t normalizedPayloadCapacity);

private:
  static constexpr size_t kAssetPathCapacity = 160;
  static constexpr size_t kMaxIndexAssets = 8;
  static constexpr size_t kUrlCapacity = 256;
  static constexpr size_t kRequestBodyCapacity = 256;
  static constexpr size_t kContentTypeCapacity = 64;

  Result discoverQueryId(const config_store::WiFiConfig &config);
  Result get(const config_store::WiFiConfig &config, const char *path,
             detail::BoundedStream &sink, size_t maxResponseBytes);
  Result postUsage(const config_store::WiFiConfig &config,
                   size_t maxResponseBytes);
  bool isJavaScriptResponse() const;
  void clearTransportDiagnostic();
  void recordTransportFailure(int httpCode);
  void resetResponseMetadata();

  WiFiClientSecure tlsClient_;
  char queryId_[kQueryIdLength + 1] = {};
  char requestUrl_[kUrlCapacity] = {};
  char requestBody_[kRequestBodyCapacity] = {};
  char contentType_[kContentTypeCapacity] = {};
  char assetPaths_[kMaxIndexAssets][kAssetPathCapacity] = {};
  size_t assetCount_ = 0;
  TransportDiagnostic transportDiagnostic_;
  bool hasTransportDiagnostic_ = false;
};

#endif // ARDUINO

} // namespace opencode_client
