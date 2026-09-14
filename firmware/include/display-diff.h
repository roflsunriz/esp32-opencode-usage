#pragma once
#include <stddef.h>
#include <stdint.h>

namespace display_diff {

constexpr size_t kWidth = 320;
constexpr size_t kHeight = 240;
constexpr size_t kBandHeight = 16;
constexpr size_t kBandCount = kHeight / kBandHeight;
static_assert(kHeight % kBandHeight == 0 && kBandCount <= 16,
              "band mask requires complete 16-bit coverage");

// TFT_eSprite 2.5.43の8-bit画像は1行320バイトの連続配置。
class Bands {
 public:
  void invalidate() { haveFrame_ = false; }
  uint16_t update(const uint8_t* pixels) {
    if (!pixels) { invalidate(); return 0; }
    uint16_t changed = 0;
    for (size_t band = 0; band < kBandCount; ++band) {
      uint64_t hash = 14695981039346656037ULL;
      const size_t first = band * kBandHeight * kWidth;
      const size_t end = first + kBandHeight * kWidth;
      for (size_t index = first; index < end; ++index) {
        hash ^= pixels[index];
        hash *= 1099511628211ULL;
      }
      if (!haveFrame_ || hash != hashes_[band])
        changed |= static_cast<uint16_t>(1U << band);
      hashes_[band] = hash;
    }
    haveFrame_ = true;
    return changed;
  }
 private:
  uint64_t hashes_[kBandCount] = {};
  bool haveFrame_ = false;
};

template <class Push>
bool eachRun(uint16_t mask, Push push) {
  for (size_t band = 0; band < kBandCount;) {
    if ((mask & (1U << band)) == 0) { ++band; continue; }
    const size_t first = band;
    do { ++band; } while (band < kBandCount && (mask & (1U << band)) != 0);
    if (!push(first * kBandHeight, (band - first) * kBandHeight)) return false;
  }
  return true;
}

}  // namespace display_diff
