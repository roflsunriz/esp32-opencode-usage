#pragma once

// ESP32-2432S028R (CYD) wiring from the board schematic/reference.
// The display uses the ESP32 VSPI peripheral routed through the GPIO matrix.
namespace board_pins {
constexpr int tft_miso = 12;
constexpr int tft_mosi = 13;
constexpr int tft_sclk = 14;
constexpr int tft_cs = 15;
constexpr int tft_dc = 2;
constexpr int tft_rst = -1; // LCD reset is tied to the board reset line.
constexpr int tft_backlight = 21;
} // namespace board_pins
