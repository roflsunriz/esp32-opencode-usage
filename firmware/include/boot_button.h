#pragma once

#include <cstdint>

namespace boot_button {

// Sample independently of HTTPS, then consume clicks on the application task.
// Both edges must be stable; holding BOOT never repeats the action.
class Model {
public:
  void start(bool pressed, uint32_t nowMs) {
    rawPressed_ = stablePressed_ = pressed;
    changedAtMs_ = nowMs;
    armed_ = false;
    pressedAtMs_ = nowMs;
    lastDurationMs_ = 0;
  }

  bool update(bool pressed, uint32_t nowMs) {
    if (pressed != rawPressed_) {
      rawPressed_ = pressed;
      changedAtMs_ = nowMs;
    }
    if (rawPressed_ == stablePressed_ ||
        static_cast<uint32_t>(nowMs - changedAtMs_) < 30) {
      return false;
    }
    stablePressed_ = rawPressed_;
    if (stablePressed_) {
      armed_ = true;
      pressedAtMs_ = nowMs;
      return false;
    }
    const bool clicked = armed_;
    if (clicked) lastDurationMs_ = static_cast<uint32_t>(nowMs - pressedAtMs_);
    armed_ = false;
    return clicked;
  }

  uint32_t lastDurationMs() const { return lastDurationMs_; }

private:
  bool rawPressed_ = false;
  bool stablePressed_ = false;
  bool armed_ = false;
  uint32_t changedAtMs_ = 0;
  uint32_t pressedAtMs_ = 0;
  uint32_t lastDurationMs_ = 0;
};

} // namespace boot_button
