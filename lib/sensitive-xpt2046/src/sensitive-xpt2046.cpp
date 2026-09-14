/*
 * Touchscreen driver for the XPT2046 controller.
 * Copyright (c) 2015 Paul Stoffregen
 * Modifications copyright (c) 2026 esp32-codex-notifications contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice, development funding notice, and this permission
 * notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "sensitive-xpt2046.h"

#include <algorithm>
#include <cstdlib>

namespace {

constexpr std::uint16_t kReleaseThreshold = 35;
constexpr std::uint32_t kReadIntervalMs = 1;
const SPISettings kSpiSettings(2000000, MSBFIRST, SPI_MODE0);
SensitiveXpt2046* activeTouchscreen = nullptr;

std::int16_t bestTwoAverage(std::int16_t first, std::int16_t second,
                            std::int16_t third) {
  const std::int16_t firstSecond = std::abs(first - second);
  const std::int16_t firstThird = std::abs(first - third);
  const std::int16_t thirdSecond = std::abs(third - second);
  if (firstSecond <= firstThird && firstSecond <= thirdSecond) {
    return static_cast<std::int16_t>((first + second) >> 1);
  }
  if (firstThird <= firstSecond && firstThird <= thirdSecond) {
    return static_cast<std::int16_t>((first + third) >> 1);
  }
  return static_cast<std::int16_t>((second + third) >> 1);
}

void IRAM_ATTR touchInterrupt() {
  if (activeTouchscreen != nullptr) activeTouchscreen->wakeFromInterrupt();
}

}  // namespace

bool SensitiveXpt2046::begin(SPIClass& spi) {
  spi_ = &spi;
  pinMode(chipSelectPin_, OUTPUT);
  digitalWrite(chipSelectPin_, HIGH);
  pinMode(interruptPin_, INPUT);
  activeTouchscreen = this;
  attachInterrupt(digitalPinToInterrupt(interruptPin_), touchInterrupt, FALLING);
  return true;
}

SensitiveTouchPoint SensitiveXpt2046::getPoint() {
  update();
  return {rawX_, rawY_, rawZ_};
}

void SensitiveXpt2046::update() {
  if (!interruptWake_ || spi_ == nullptr) return;
  const std::uint32_t now = millis();
  if (now - lastReadMs_ < kReadIntervalMs) return;

  std::int16_t data[6] = {};
  spi_->beginTransaction(kSpiSettings);
  digitalWrite(chipSelectPin_, LOW);
  spi_->transfer(0xB1);
  const std::int16_t z1 = spi_->transfer16(0xC1) >> 3;
  std::int16_t pressure = static_cast<std::int16_t>(z1 + 4095);
  const std::int16_t z2 = spi_->transfer16(0x91) >> 3;
  pressure = static_cast<std::int16_t>(pressure - z2);
  if (pressure >= static_cast<std::int16_t>(pressureThreshold_)) {
    spi_->transfer16(0x91);
    data[0] = spi_->transfer16(0xD1) >> 3;
    data[1] = spi_->transfer16(0x91) >> 3;
    data[2] = spi_->transfer16(0xD1) >> 3;
    data[3] = spi_->transfer16(0x91) >> 3;
  }
  data[4] = spi_->transfer16(0xD0) >> 3;
  data[5] = spi_->transfer16(0) >> 3;
  digitalWrite(chipSelectPin_, HIGH);
  spi_->endTransaction();

  pressure = std::max<std::int16_t>(0, pressure);
  if (pressure < static_cast<std::int16_t>(pressureThreshold_)) {
    rawZ_ = 0;
    if (pressure < static_cast<std::int16_t>(kReleaseThreshold)) interruptWake_ = false;
    return;
  }

  rawZ_ = pressure;
  lastReadMs_ = now;
  const std::int16_t x = bestTwoAverage(data[0], data[2], data[4]);
  const std::int16_t y = bestTwoAverage(data[1], data[3], data[5]);
  if (rotation_ == 0) {
    rawX_ = static_cast<std::int16_t>(4095 - y);
    rawY_ = x;
  } else if (rotation_ == 1) {
    rawX_ = x;
    rawY_ = y;
  } else if (rotation_ == 2) {
    rawX_ = y;
    rawY_ = static_cast<std::int16_t>(4095 - x);
  } else {
    rawX_ = static_cast<std::int16_t>(4095 - x);
    rawY_ = static_cast<std::int16_t>(4095 - y);
  }
}
