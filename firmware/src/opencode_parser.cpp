#include "opencode_client.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace opencode_client {
namespace {

// Console status amounts are integer microCents: 1 cent = 1,000,000, so
// 1 dollar = 100,000,000. They are billing-weighted quota units,
// not invoice amounts.
constexpr uint64_t kMicroCentsPerDollar = 100000000ULL;
constexpr double kMaxSafeInteger = 9007199254740991.0;
constexpr size_t kMaxDigitsLength = 24;

bool isWhitespace(char value) {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

bool isDigit(char value) { return value >= '0' && value <= '9'; }

void skipWhitespace(const char *input, size_t length, size_t &position) {
  while (position < length && isWhitespace(input[position]))
    ++position;
}

// Finds `"key"` followed by `:` and returns the position just after the colon.
bool findKeyValue(const char *input, size_t length, const char *key,
                  size_t &valuePosition) {
  const size_t keyLength = strlen(key);
  for (size_t index = 0; index + keyLength + 2 <= length; ++index) {
    if (input[index] != '"')
      continue;
    if (memcmp(input + index + 1, key, keyLength) != 0 ||
        input[index + keyLength + 1] != '"') {
      continue;
    }
    size_t position = index + keyLength + 2;
    skipWhitespace(input, length, position);
    if (position < length && input[position] == ':') {
      ++position;
      skipWhitespace(input, length, position);
      valuePosition = position;
      return true;
    }
  }
  return false;
}

// Matches a `{...}` object starting at an opening brace, honoring strings.
bool matchObject(const char *input, size_t length, size_t openPosition,
                 size_t &contentBegin, size_t &contentEnd) {
  if (openPosition >= length || input[openPosition] != '{')
    return false;
  size_t position = openPosition + 1;
  int depth = 1;
  bool inString = false;
  while (position < length) {
    const char value = input[position];
    if (inString) {
      if (value == '\\') {
        position += 2;
        continue;
      }
      if (value == '"')
        inString = false;
      ++position;
      continue;
    }
    if (value == '"') {
      inString = true;
    } else if (value == '{') {
      ++depth;
    } else if (value == '}') {
      if (--depth == 0) {
        contentBegin = openPosition + 1;
        contentEnd = position;
        return true;
      }
    }
    ++position;
  }
  return false;
}

bool findObject(const char *input, size_t begin, size_t end, const char *key,
                size_t &contentBegin, size_t &contentEnd) {
  const size_t keyLength = strlen(key);
  for (size_t index = begin; index + keyLength + 2 <= end;) {
    if (input[index] != '"') {
      ++index;
      continue;
    }
    if (memcmp(input + index + 1, key, keyLength) != 0 ||
        input[index + keyLength + 1] != '"') {
      ++index;
      continue;
    }
    size_t position = index + keyLength + 2;
    while (position < end && isWhitespace(input[position]))
      ++position;
    if (position >= end || input[position] != ':') {
      ++index;
      continue;
    }
    ++position;
    while (position < end && isWhitespace(input[position]))
      ++position;
    if (position < end && input[position] == '{' &&
        matchObject(input, end, position, contentBegin, contentEnd)) {
      return true;
    }
    ++index;
  }
  return false;
}

// Reads a JSON string value (without escape processing beyond validation).
bool readJsonString(const char *input, size_t begin, size_t end,
                    size_t &textBegin, size_t &textEnd) {
  if (begin >= end || input[begin] != '"')
    return false;
  size_t position = begin + 1;
  while (position < end) {
    const char value = input[position];
    if (value == '\\') {
      position += 2;
      continue;
    }
    if (value == '"') {
      textBegin = begin + 1;
      textEnd = position;
      return true;
    }
    // Control characters must be escaped in JSON.
    if (value < 0x20)
      return false;
    ++position;
  }
  return false;
}

// Parses an integer microCent amount given as a JSON string or number.
bool parseMicroCents(const char *input, size_t begin, size_t end,
                     uint64_t &value) {
  size_t numberBegin = begin;
  size_t numberEnd = end;
  if (begin < end && input[begin] == '"') {
    if (!readJsonString(input, begin, end, numberBegin, numberEnd))
      return false;
  } else {
    size_t position = begin;
    while (position < end && isDigit(input[position]))
      ++position;
    if (position == begin || position != end)
      return false;
    numberBegin = begin;
    numberEnd = position;
  }
  const size_t tokenLength = numberEnd - numberBegin;
  if (tokenLength == 0 || tokenLength > kMaxDigitsLength)
    return false;
  // Reject leading zeros such as "007" while allowing "0" itself.
  if (tokenLength > 1 && input[numberBegin] == '0')
    return false;
  char number[kMaxDigitsLength + 1] = {};
  memcpy(number, input + numberBegin, tokenLength);
  char *stop = nullptr;
  const double parsed = strtod(number, &stop);
  if (stop != number + tokenLength || !std::isfinite(parsed) || parsed < 0.0 ||
      std::floor(parsed) != parsed || parsed > kMaxSafeInteger) {
    return false;
  }
  value = static_cast<uint64_t>(parsed);
  return true;
}

bool findRawValue(const char *input, size_t begin, size_t end, const char *key,
                  size_t &valueBegin, size_t &valueEnd) {
  size_t keyLength = strlen(key);
  for (size_t index = begin; index + keyLength + 2 <= end;) {
    if (input[index] != '"') {
      ++index;
      continue;
    }
    if (memcmp(input + index + 1, key, keyLength) != 0 ||
        input[index + keyLength + 1] != '"') {
      ++index;
      continue;
    }
    size_t position = index + keyLength + 2;
    while (position < end && isWhitespace(input[position]))
      ++position;
    if (position >= end || input[position] != ':') {
      ++index;
      continue;
    }
    ++position;
    while (position < end && isWhitespace(input[position]))
      ++position;
    if (position >= end)
      return false;
    valueBegin = position;
    if (input[position] == '"') {
      size_t textBegin = 0;
      size_t textEnd = 0;
      if (!readJsonString(input, position, end, textBegin, textEnd))
        return false;
      valueEnd = textEnd + 1; // Include the closing quote.
    } else {
      size_t cursor = position;
      while (cursor < end && input[cursor] != ',' && input[cursor] != '}' &&
             !isWhitespace(input[cursor]))
        ++cursor;
      if (cursor == position)
        return false;
      valueEnd = cursor;
    }
    return true;
  }
  return false;
}

// Parses "YYYY-MM-DDTHH:MM:SS[.fff]Z" into Unix epoch seconds.
bool parseIso8601Utc(const char *input, size_t begin, size_t end,
                     uint64_t &epochSec) {
  // Shortest accepted form: "1970-01-01T00:00:00Z" (20 chars).
  if (end < begin || end - begin < 20 || end - begin > 28)
    return false;
  if (input[begin + 4] != '-' || input[begin + 7] != '-')
    return false;
  if (input[begin + 10] != 'T')
    return false;
  if (input[begin + 13] != ':' || input[begin + 16] != ':')
    return false;
  if (input[end - 1] != 'Z')
    return false;
  for (size_t index = begin; index < end - 1; ++index) {
    if (index == begin + 4 || index == begin + 7 || index == begin + 10 ||
        index == begin + 13 || index == begin + 16) {
      continue;
    }
    if (index >= begin + 19 && input[index] == '.')
      continue;
    if (!isDigit(input[index]))
      return false;
  }
  const int year = (input[begin] - '0') * 1000 + (input[begin + 1] - '0') * 100 +
                   (input[begin + 2] - '0') * 10 + (input[begin + 3] - '0');
  const int month = (input[begin + 5] - '0') * 10 + (input[begin + 6] - '0');
  const int day = (input[begin + 8] - '0') * 10 + (input[begin + 9] - '0');
  const int hour = (input[begin + 11] - '0') * 10 + (input[begin + 12] - '0');
  const int minute = (input[begin + 14] - '0') * 10 + (input[begin + 15] - '0');
  const int second = (input[begin + 17] - '0') * 10 + (input[begin + 18] - '0');
  if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 ||
      hour > 23 || minute > 59 || second > 60) {
    return false;
  }
  // Days-from-civil algorithm; valid for whole proleptic Gregorian range.
  const int adjustedYear = month <= 2 ? year - 1 : year;
  const int era = (adjustedYear >= 0 ? adjustedYear : adjustedYear - 399) / 400;
  const unsigned yearOfEra =
      static_cast<unsigned>(adjustedYear - era * 400);
  const unsigned monthIndex = static_cast<unsigned>((month + 9) % 12);
  const unsigned dayIndex = static_cast<unsigned>(day - 1);
  const unsigned dayOfYear =
      (153 * monthIndex + 2) / 5 + dayIndex;
  const unsigned dayOfEra =
      yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  const int64_t days =
      static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(dayOfEra) -
      719468;
  // Reject impossible month/day combinations via round-trip check.
  const int64_t shifted = days + 719468;
  const int64_t checkEra =
      (shifted >= 0 ? shifted : shifted - 146096) / 146097;
  const unsigned checkDayOfEra =
      static_cast<unsigned>(shifted - checkEra * 146097);
  const unsigned checkYearOfEra =
      (checkDayOfEra - checkDayOfEra / 1460 + checkDayOfEra / 36524 -
       checkDayOfEra / 146096) /
      365;
  const unsigned checkYear =
      checkYearOfEra + static_cast<unsigned>(checkEra) * 400;
  const unsigned checkDayOfYear =
      checkDayOfEra -
      (365 * checkYearOfEra + checkYearOfEra / 4 - checkYearOfEra / 100);
  const unsigned checkMonth =
      (5 * checkDayOfYear + 2) / 153;
  const unsigned checkDay =
      checkDayOfYear - (153 * checkMonth + 2) / 5 + 1;
  const unsigned resolvedMonth =
      checkMonth < 10 ? checkMonth + 3 : checkMonth - 9;
  const unsigned resolvedYear =
      resolvedMonth <= 2 ? checkYear + 1 : checkYear;
  if (resolvedYear != static_cast<unsigned>(year) ||
      resolvedMonth != static_cast<unsigned>(month) ||
      checkDay != static_cast<unsigned>(day)) {
    return false;
  }
  epochSec = static_cast<uint64_t>(days) * 86400ULL +
             static_cast<uint64_t>(hour) * 3600ULL +
             static_cast<uint64_t>(minute) * 60ULL +
             static_cast<uint64_t>(second);
  return true;
}

struct MeterData {
  uint64_t usedMicro = 0;
  uint64_t limitMicro = 0;
  uint64_t resetEpochSec = 0;
};

bool parseMeter(const char *input, size_t begin, size_t end,
                size_t fallbackBegin, size_t fallbackEnd, bool hasFallback,
                MeterData &meter) {
  size_t valueBegin = 0;
  size_t valueEnd = 0;
  if (!findRawValue(input, begin, end, "usedMicroCents", valueBegin,
                    valueEnd) ||
      !parseMicroCents(input, valueBegin, valueEnd, meter.usedMicro)) {
    return false;
  }
  if (!findRawValue(input, begin, end, "limitMicroCents", valueBegin,
                    valueEnd) ||
      !parseMicroCents(input, valueBegin, valueEnd, meter.limitMicro) ||
      meter.limitMicro == 0) {
    return false;
  }
  if (findRawValue(input, begin, end, "resetsAt", valueBegin, valueEnd)) {
    size_t textBegin = 0;
    size_t textEnd = 0;
    if (valueBegin >= valueEnd || input[valueBegin] != '"' ||
        !readJsonString(input, valueBegin, valueEnd, textBegin, textEnd) ||
        !parseIso8601Utc(input, textBegin, textEnd, meter.resetEpochSec)) {
      return false;
    }
  } else {
    // The monthly meter carries no resetsAt; the subscription endsAt applies.
    if (!hasFallback)
      return false;
    size_t textBegin = 0;
    size_t textEnd = 0;
    if (!readJsonString(input, fallbackBegin, fallbackEnd, textBegin,
                        textEnd) ||
        !parseIso8601Utc(input, textBegin, textEnd, meter.resetEpochSec)) {
      return false;
    }
  }
  return true;
}

} // namespace

bool normalizeConsoleStatusUsage(const char *response, size_t length,
                                 uint64_t updatedAtSec, char *output,
                                 size_t outputCapacity) {
  if (response == nullptr || output == nullptr || outputCapacity == 0) {
    return false;
  }
  output[0] = '\0';
  size_t accessBegin = 0;
  size_t accessEnd = 0;
  {
    size_t valuePosition = 0;
    if (!findKeyValue(response, length, "access", valuePosition) ||
        !matchObject(response, length, valuePosition, accessBegin,
                     accessEnd)) {
      return false;
    }
  }
  size_t metersBegin = 0;
  size_t metersEnd = 0;
  if (!findObject(response, accessBegin, accessEnd, "meters", metersBegin,
                  metersEnd)) {
    return false;
  }
  size_t endsAtBegin = 0;
  size_t endsAtEnd = 0;
  if (!findRawValue(response, accessBegin, accessEnd, "endsAt", endsAtBegin,
                    endsAtEnd) ||
      endsAtBegin >= endsAtEnd || response[endsAtBegin] != '"') {
    return false;
  }
  MeterData meters[3] = {};
  const char *names[3] = {"fiveHour", "week", "month"};
  for (int index = 0; index < 3; ++index) {
    size_t contentBegin = 0;
    size_t contentEnd = 0;
    if (!findObject(response, metersBegin, metersEnd, names[index],
                    contentBegin, contentEnd)) {
      return false;
    }
    const bool monthly = index == 2;
    if (!parseMeter(response, contentBegin, contentEnd, endsAtBegin, endsAtEnd,
                    monthly, meters[index])) {
      return false;
    }
  }
  double used[3] = {};
  double limit[3] = {};
  double percent[3] = {};
  uint64_t resetInSec[3] = {};
  for (int index = 0; index < 3; ++index) {
    used[index] =
        static_cast<double>(meters[index].usedMicro) /
        static_cast<double>(kMicroCentsPerDollar);
    limit[index] =
        static_cast<double>(meters[index].limitMicro) /
        static_cast<double>(kMicroCentsPerDollar);
    percent[index] = (static_cast<double>(meters[index].usedMicro) /
                      static_cast<double>(meters[index].limitMicro)) *
                     100.0;
    if (!std::isfinite(percent[index]) || percent[index] < 0.0 ||
        percent[index] > 100000.0) {
      return false;
    }
    resetInSec[index] = meters[index].resetEpochSec >= updatedAtSec
                            ? meters[index].resetEpochSec - updatedAtSec
                            : 0;
  }
  const int written = snprintf(
      output, outputCapacity,
      "{\"version\":1,\"type\":\"usage\",\"updatedAt\":%llu,"
      "\"rolling\":{\"used\":%.8f,\"limit\":%.8f,\"percent\":%.6g,"
      "\"resetInSec\":%llu},\"weekly\":{\"used\":%.8f,\"limit\":%.8f,"
      "\"percent\":%.6g,\"resetInSec\":%llu},\"monthly\":{\"used\":%.8f,"
      "\"limit\":%.8f,\"percent\":%.6g,\"resetInSec\":%llu}}",
      static_cast<unsigned long long>(updatedAtSec), used[0], limit[0],
      percent[0], static_cast<unsigned long long>(resetInSec[0]), used[1],
      limit[1], percent[1], static_cast<unsigned long long>(resetInSec[1]),
      used[2], limit[2], percent[2],
      static_cast<unsigned long long>(resetInSec[2]));
  if (written < 0 || static_cast<size_t>(written) >= outputCapacity) {
    output[0] = '\0';
    return false;
  }
  return true;
}

} // namespace opencode_client
