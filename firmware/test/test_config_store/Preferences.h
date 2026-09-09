#pragma once

#include <cstddef>
#include <cstdint>

class Preferences {
public:
  bool begin(const char *name, bool readOnly = false);
  size_t getBytesLength(const char *key) const;
  size_t getBytes(const char *key, void *value, size_t length) const;
  size_t putBytes(const char *key, const void *value, size_t length);
  bool remove(const char *key);

  static void setFixture(const void *value, size_t length);
  static void setWriteFailure(bool enabled);
  static const uint8_t *data();
  static size_t size();
  static void clear();

private:
  static constexpr size_t kCapacity = 5000;
  static uint8_t storage_[kCapacity];
  static size_t length_;
  static bool writeFailure_;
};
