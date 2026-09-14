/*
 * Adapted from Paul Stoffregen's XPT2046_Touchscreen v1.4 (MIT).
 * The pressure threshold is configurable for resistive stylus input.
 */
#pragma once

#include <Arduino.h>
#include <SPI.h>

#include <cstdint>

struct SensitiveTouchPoint {
  SensitiveTouchPoint() = default;
  SensitiveTouchPoint(std::int16_t xValue, std::int16_t yValue, std::int16_t zValue)
      : x(xValue), y(yValue), z(zValue) {}

  std::int16_t x = 0;
  std::int16_t y = 0;
  std::int16_t z = 0;
};

class SensitiveXpt2046 {
 public:
  SensitiveXpt2046(std::uint8_t chipSelectPin, std::uint8_t interruptPin,
                   std::uint16_t pressureThreshold)
      : chipSelectPin_(chipSelectPin),
        interruptPin_(interruptPin),
        pressureThreshold_(pressureThreshold) {}

  bool begin(SPIClass& spi);
  void setRotation(std::uint8_t rotation) { rotation_ = rotation % 4; }
  void setPressureThreshold(std::uint16_t threshold) { pressureThreshold_ = threshold; }
  bool tirqTouched() const { return interruptWake_; }
  SensitiveTouchPoint getPoint();
  void setInterruptCallback(void (*callback)(void*), void* context) {
    interruptCallback_ = callback;
    interruptContext_ = context;
  }
  void wakeFromInterrupt() {
    interruptWake_ = true;
    if (interruptCallback_) interruptCallback_(interruptContext_);
  }

 private:
  void update();

  SPIClass* spi_ = nullptr;
  std::uint8_t chipSelectPin_;
  std::uint8_t interruptPin_;
  std::uint8_t rotation_ = 1;
  std::uint16_t pressureThreshold_;
  volatile bool interruptWake_ = true;
  std::int16_t rawX_ = 0;
  std::int16_t rawY_ = 0;
  std::int16_t rawZ_ = 0;
  std::uint32_t lastReadMs_ = 0x80000000UL;
  void (*interruptCallback_)(void*) = nullptr;
  void* interruptContext_ = nullptr;
};
