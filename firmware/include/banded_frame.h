#pragma once

#include <cstdint>

namespace banded_frame {

struct Rect {
  Rect() : x(0), y(0), width(0), height(0), visible(false) {}
  Rect(int16_t xValue, int16_t yValue, int16_t widthValue,
       int16_t heightValue, bool visibleValue)
      : x(xValue), y(yValue), width(widthValue), height(heightValue),
        visible(visibleValue) {}

  int16_t x;
  int16_t y;
  int16_t width;
  int16_t height;
  bool visible;
};

// Returns only the part of a screen-space rectangle that belongs to one
// scanline band. The 32-bit intermediates keep partially off-screen shapes
// safe when Adafruit_GFX supplies negative coordinates.
inline Rect clipToBand(int16_t x, int16_t y, int16_t width, int16_t height,
                       int16_t screenWidth, int16_t screenHeight,
                       int16_t bandTop, int16_t bandHeight) {
  if (width <= 0 || height <= 0 || screenWidth <= 0 || screenHeight <= 0 ||
      bandHeight <= 0) {
    return Rect{};
  }

  const int32_t left = x < 0 ? 0 : x;
  const int32_t right =
      static_cast<int32_t>(x) + width < screenWidth
          ? static_cast<int32_t>(x) + width
          : screenWidth;
  const int32_t top =
      y < bandTop ? bandTop : (y < 0 ? 0 : static_cast<int32_t>(y));
  const int32_t bandBottom = static_cast<int32_t>(bandTop) + bandHeight;
  const int32_t bottom = static_cast<int32_t>(y) + height < screenHeight
                             ? static_cast<int32_t>(y) + height
                             : screenHeight;
  const int32_t clippedBottom = bottom < bandBottom ? bottom : bandBottom;
  if (right <= left || clippedBottom <= top) {
    return Rect{};
  }

  return Rect(static_cast<int16_t>(left), static_cast<int16_t>(top),
              static_cast<int16_t>(right - left),
              static_cast<int16_t>(clippedBottom - top), true);
}

} // namespace banded_frame
