#include "opencode_client.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace opencode_client {
namespace {

constexpr uint64_t kUnitsPerDollar = 100000000ULL;
constexpr uint64_t kMaxSafeInteger = 9007199254740991ULL;
constexpr size_t kMaxNumberTokenLength = 48;

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

void skipWhitespace(const char *input, size_t length, size_t &position) {
  while (position < length && isWhitespace(input[position]))
    ++position;
}

bool equals(const char *input, size_t length, const char *literal) {
  return length == strlen(literal) && memcmp(input, literal, length) == 0;
}

bool matchesAt(const char *input, size_t length, size_t position,
               const char *literal) {
  const size_t literalLength = strlen(literal);
  return position <= length && literalLength <= length - position &&
         memcmp(input + position, literal, literalLength) == 0;
}

bool hasIdentifierBoundary(const char *input, size_t length, size_t position,
                           size_t tokenLength) {
  return (position == 0 || !isIdentifierCharacter(input[position - 1])) &&
         (position + tokenLength == length ||
          !isIdentifierCharacter(input[position + tokenLength]));
}

struct NumberToken {
  const char *text = nullptr;
  size_t length = 0;
  double value = 0.0;
};

bool parseNumberToken(const char *input, size_t length, size_t &position,
                      NumberToken &result) {
  const size_t start = position;
  size_t cursor = position;
  size_t digits = 0;
  while (cursor < length && input[cursor] >= '0' && input[cursor] <= '9') {
    ++cursor;
    ++digits;
  }
  if (digits == 0)
    return false;
  if (cursor < length && input[cursor] == '.') {
    ++cursor;
    size_t fractionDigits = 0;
    while (cursor < length && input[cursor] >= '0' && input[cursor] <= '9') {
      ++cursor;
      ++fractionDigits;
    }
    if (fractionDigits == 0)
      return false;
  }
  if (cursor < length && (input[cursor] == 'e' || input[cursor] == 'E')) {
    ++cursor;
    if (cursor < length && (input[cursor] == '+' || input[cursor] == '-')) {
      ++cursor;
    }
    size_t exponentDigits = 0;
    while (cursor < length && input[cursor] >= '0' && input[cursor] <= '9') {
      ++cursor;
      ++exponentDigits;
    }
    if (exponentDigits == 0)
      return false;
  }
  const size_t tokenLength = cursor - start;
  if (tokenLength == 0 || tokenLength > kMaxNumberTokenLength)
    return false;
  char number[kMaxNumberTokenLength + 1] = {};
  memcpy(number, input + start, tokenLength);
  char *end = nullptr;
  const double value = strtod(number, &end);
  if (end != number + tokenLength || !std::isfinite(value) || value < 0.0) {
    return false;
  }
  result.text = input + start;
  result.length = tokenLength;
  result.value = value;
  position = cursor;
  return true;
}

bool parseSafeInteger(const char *input, size_t length, size_t &position,
                      uint64_t &value) {
  NumberToken token;
  if (!parseNumberToken(input, length, position, token) ||
      std::floor(token.value) != token.value ||
      token.value > static_cast<double>(kMaxSafeInteger)) {
    return false;
  }
  value = static_cast<uint64_t>(token.value);
  return true;
}

bool readPropertyName(const char *input, size_t length, size_t &position,
                      const char *&name, size_t &nameLength) {
  skipWhitespace(input, length, position);
  const size_t start = position;
  while (position < length && isIdentifierCharacter(input[position]))
    ++position;
  if (position == start)
    return false;
  name = input + start;
  nameLength = position - start;
  return true;
}

bool skipUnknownValue(const char *input, size_t length, size_t &position) {
  if (position >= length)
    return false;
  const char quote = input[position];
  if (quote == '\'' || quote == '"') {
    ++position;
    while (position < length) {
      const char value = input[position++];
      if (value == '\\') {
        if (position >= length)
          return false;
        ++position;
      } else if (value == quote) {
        return true;
      }
    }
    return false;
  }
  const size_t start = position;
  while (position < length && input[position] != ',' &&
         input[position] != '}') {
    const char value = input[position];
    if (value == '{' || value == '[' || value == ']' || value == '\'' ||
        value == '"') {
      return false;
    }
    ++position;
  }
  return position != start;
}

struct PeriodData {
  uint64_t usage = 0;
  uint64_t limit = 0;
  uint64_t resetInSec = 0;
  NumberToken usagePercent;
};

bool parsePeriodObject(const char *input, size_t length, size_t &position,
                       PeriodData &period) {
  if (position >= length || input[position] != '{')
    return false;
  ++position;
  bool hasUsage = false;
  bool hasLimit = false;
  bool hasPercent = false;
  bool hasReset = false;
  while (position < length) {
    skipWhitespace(input, length, position);
    if (position < length && input[position] == '}') {
      ++position;
      break;
    }
    const char *name = nullptr;
    size_t nameLength = 0;
    if (!readPropertyName(input, length, position, name, nameLength))
      return false;
    skipWhitespace(input, length, position);
    if (position >= length || input[position] != ':')
      return false;
    ++position;
    skipWhitespace(input, length, position);
    if (equals(name, nameLength, "usage")) {
      if (hasUsage ||
          !parseSafeInteger(input, length, position, period.usage)) {
        return false;
      }
      hasUsage = true;
    } else if (equals(name, nameLength, "limit")) {
      if (hasLimit ||
          !parseSafeInteger(input, length, position, period.limit)) {
        return false;
      }
      hasLimit = true;
    } else if (equals(name, nameLength, "usagePercent")) {
      if (hasPercent ||
          !parseNumberToken(input, length, position, period.usagePercent) ||
          period.usagePercent.value > 100000.0) {
        return false;
      }
      hasPercent = true;
    } else if (equals(name, nameLength, "resetInSec")) {
      if (hasReset ||
          !parseSafeInteger(input, length, position, period.resetInSec)) {
        return false;
      }
      hasReset = true;
    } else if (!skipUnknownValue(input, length, position)) {
      return false;
    }
    skipWhitespace(input, length, position);
    if (position >= length)
      return false;
    if (input[position] == ',') {
      ++position;
      continue;
    }
    if (input[position] == '}') {
      ++position;
      break;
    }
    return false;
  }
  if (!hasUsage || !hasLimit || !hasPercent || !hasReset || period.limit == 0) {
    return false;
  }
  const double expectedPercent =
      (static_cast<double>(period.usage) / static_cast<double>(period.limit)) *
      100.0;
  return std::fabs(period.usagePercent.value - expectedPercent) <= 0.11;
}

bool findPeriod(const char *input, size_t length, const char *periodName,
                PeriodData &period) {
  const size_t nameLength = strlen(periodName);
  size_t matches = 0;
  for (size_t index = 0; index + nameLength <= length; ++index) {
    if (!matchesAt(input, length, index, periodName) ||
        !hasIdentifierBoundary(input, length, index, nameLength)) {
      continue;
    }
    size_t position = index + nameLength;
    skipWhitespace(input, length, position);
    if (position >= length || input[position] != ':')
      continue;
    ++position;
    skipWhitespace(input, length, position);
    if (matchesAt(input, length, position, "$R[")) {
      position += 3;
      const size_t digitsStart = position;
      while (position < length && input[position] >= '0' &&
             input[position] <= '9') {
        ++position;
      }
      if (position == digitsStart || position >= length ||
          input[position] != ']') {
        return false;
      }
      ++position;
      skipWhitespace(input, length, position);
      if (position >= length || input[position] != '=')
        return false;
      ++position;
      skipWhitespace(input, length, position);
    }
    if (position >= length || input[position] != '{')
      return false;
    PeriodData candidate;
    if (!parsePeriodObject(input, length, position, candidate) ||
        ++matches > 1) {
      return false;
    }
    period = candidate;
    index = position - 1;
  }
  return matches == 1;
}

} // namespace

bool extractQueryIdFromScript(const char *script, size_t length, char *queryId,
                              size_t queryIdCapacity) {
  if (script == nullptr || queryId == nullptr ||
      queryIdCapacity < kQueryIdLength + 1) {
    return false;
  }
  queryId[0] = '\0';
  constexpr char kReferenceName[] = "queryLiteSubscription_query";
  constexpr size_t kReferenceNameLength = sizeof(kReferenceName) - 1;
  size_t matches = 0;
  for (size_t index = 0; index + kReferenceNameLength <= length; ++index) {
    if (!matchesAt(script, length, index, kReferenceName) ||
        !hasIdentifierBoundary(script, length, index, kReferenceNameLength)) {
      continue;
    }
    size_t position = index + kReferenceNameLength;
    skipWhitespace(script, length, position);
    if (position >= length || script[position] != '=')
      continue;
    ++position;
    skipWhitespace(script, length, position);
    const size_t functionStart = position;
    while (position < length && isIdentifierCharacter(script[position]))
      ++position;
    if (position == functionStart)
      continue;
    skipWhitespace(script, length, position);
    if (position >= length || script[position] != '(')
      continue;
    ++position;
    skipWhitespace(script, length, position);
    if (position >= length ||
        (script[position] != '\'' && script[position] != '"')) {
      continue;
    }
    const char quote = script[position++];
    if (kQueryIdLength > length - position)
      continue;
    bool hexadecimal = true;
    for (size_t digit = 0; digit < kQueryIdLength; ++digit) {
      hexadecimal = hexadecimal && isLowerHex(script[position + digit]);
    }
    if (!hexadecimal)
      continue;
    const size_t queryStart = position;
    position += kQueryIdLength;
    if (position >= length || script[position] != quote)
      continue;
    ++position;
    skipWhitespace(script, length, position);
    if (position >= length || script[position] != ')')
      continue;
    if (++matches != 1) {
      queryId[0] = '\0';
      return false;
    }
    memcpy(queryId, script + queryStart, kQueryIdLength);
    queryId[kQueryIdLength] = '\0';
  }
  return matches == 1;
}

bool normalizeSerovalUsage(const char *response, size_t length,
                           uint64_t updatedAt, char *output,
                           size_t outputCapacity) {
  if (response == nullptr || output == nullptr || outputCapacity == 0) {
    return false;
  }
  output[0] = '\0';
  PeriodData rolling;
  PeriodData weekly;
  PeriodData monthly;
  if (!findPeriod(response, length, "rollingUsage", rolling) ||
      !findPeriod(response, length, "weeklyUsage", weekly) ||
      !findPeriod(response, length, "monthlyUsage", monthly)) {
    return false;
  }
  const int written = snprintf(
      output, outputCapacity,
      "{\"version\":1,\"type\":\"usage\",\"updatedAt\":%llu,"
      "\"rolling\":{\"used\":%.8f,\"limit\":%.8f,\"percent\":%.*s,"
      "\"resetInSec\":%llu},\"weekly\":{\"used\":%.8f,\"limit\":%.8f,"
      "\"percent\":%.*s,\"resetInSec\":%llu},\"monthly\":{\"used\":%.8f,"
      "\"limit\":%.8f,\"percent\":%.*s,\"resetInSec\":%llu}}",
      static_cast<unsigned long long>(updatedAt),
      static_cast<double>(rolling.usage) / static_cast<double>(kUnitsPerDollar),
      static_cast<double>(rolling.limit) / static_cast<double>(kUnitsPerDollar),
      static_cast<int>(rolling.usagePercent.length), rolling.usagePercent.text,
      static_cast<unsigned long long>(rolling.resetInSec),
      static_cast<double>(weekly.usage) / static_cast<double>(kUnitsPerDollar),
      static_cast<double>(weekly.limit) / static_cast<double>(kUnitsPerDollar),
      static_cast<int>(weekly.usagePercent.length), weekly.usagePercent.text,
      static_cast<unsigned long long>(weekly.resetInSec),
      static_cast<double>(monthly.usage) / static_cast<double>(kUnitsPerDollar),
      static_cast<double>(monthly.limit) / static_cast<double>(kUnitsPerDollar),
      static_cast<int>(monthly.usagePercent.length), monthly.usagePercent.text,
      static_cast<unsigned long long>(monthly.resetInSec));
  if (written < 0 || static_cast<size_t>(written) >= outputCapacity) {
    output[0] = '\0';
    return false;
  }
  return true;
}

} // namespace opencode_client
