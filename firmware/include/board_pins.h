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
// The board schematic's XPT2046 /PENIRQ -> TP_IRQ -> IO36 path has an
// external 10 kOhm pull-up, so GPIO36 is input-only and asserts LOW on touch.
// Source: ESP32-2432S028 circuit schematic, XPT2046 U3 /PENIRQ section.
constexpr int touch_irq = 36;
// The same U3 section routes the separate touch SPI bus used for filtered
// XPT2046 coordinate reads and PENIRQ power-down restoration.
constexpr int touch_mosi = 32;
constexpr int touch_miso = 39;
constexpr int touch_sclk = 25;
constexpr int touch_cs = 33;
} // namespace board_pins
