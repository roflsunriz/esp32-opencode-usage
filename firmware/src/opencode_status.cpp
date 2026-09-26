#include "opencode_client.h"

#include <cstdio>
#include <cstring>

namespace opencode_client {

const char *responseStatusText(Result result) {
  switch (result) {
  case opencode_client::Result::kAuthenticationRequired:
    return "OpenCode auth failed - login";
  case opencode_client::Result::kQueryRejected:
  case opencode_client::Result::kDiscoveryFailed:
    return "OpenCode query invalid";
  case opencode_client::Result::kPayloadTooLarge:
    return "OpenCode response too large";
  case opencode_client::Result::kResponseInvalid:
    return "OpenCode response invalid";
  case opencode_client::Result::kTransportFailed:
    return "OpenCode HTTPS failed";
  case opencode_client::Result::kOk:
    return "OpenCode updated";
  }
  return "OpenCode response invalid";
}

bool isTruncatedBody(int contentLength, size_t receivedBytes) {
  return contentLength > 0 &&
         receivedBytes != static_cast<size_t>(contentLength);
}

size_t formatResponseDiagnostic(char *output, size_t capacity,
                                const char *result,
                                const ResponseDiagnostic &diagnostic) {
  if (output == nullptr || capacity == 0 || result == nullptr ||
      result[0] == '\0' || strlen(result) > 16) {
    return 0;
  }
  char safeResult[17] = {};
  size_t resultLength = 0;
  for (const char *cursor = result; *cursor != '\0'; ++cursor) {
    const char value = *cursor;
    const bool allowed = (value >= 'a' && value <= 'z') || value == '_';
    if (!allowed) {
      return 0;
    }
    safeResult[resultLength++] = value;
  }
  const int written = snprintf(
      output, capacity,
      "{\"version\":1,\"type\":\"diagnostic\",\"component\":"
      "\"opencode_response\",\"result\":\"%s\",\"httpCode\":%d,"
      "\"contentLength\":%d,\"bodyBytes\":%u,\"contentTypeLength\":%u,"
      "\"isJson\":%s,\"isHtml\":%s}",
      safeResult, diagnostic.httpCode, diagnostic.contentLength,
      diagnostic.bodyBytes, diagnostic.contentTypeLength,
      diagnostic.isJson ? "true" : "false",
      diagnostic.isHtml ? "true" : "false");
  if (written < 0 || static_cast<size_t>(written) >= capacity) {
    if (capacity > 0) {
      output[0] = '\0';
    }
    return 0;
  }
  return static_cast<size_t>(written);
}

} // namespace opencode_client
