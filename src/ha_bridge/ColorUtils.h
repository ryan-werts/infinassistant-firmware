#pragma once

#include <cstdint>
#include <lvgl/lvgl.h>

namespace Pinetime {
namespace Applications {
namespace HaBridge {

/**
 * Parse a "#RRGGBB" hex color string into an lv_color_t.
 * Returns LV_COLOR_GRAY for null, empty, or missing '#' prefix.
 * Handles both upper and lowercase hex digits.
 */
inline lv_color_t ParseColor(const char* hexColor) {
  if (hexColor == nullptr || hexColor[0] != '#') return LV_COLOR_GRAY;
  const char* hex = hexColor + 1;
  uint32_t color = 0;
  for (int i = 0; hex[i] != '\0' && i < 6; i++) {
    char c = hex[i];
    uint8_t val = 0;
    if (c >= '0' && c <= '9') val = c - '0';
    else if (c >= 'a' && c <= 'f') val = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') val = c - 'A' + 10;
    color = (color << 4) | val;
  }
  uint8_t r = (color >> 16) & 0xFF;
  uint8_t g = (color >> 8) & 0xFF;
  uint8_t b = color & 0xFF;
  return LV_COLOR_MAKE(r, g, b);
}

/**
 * Determine a high-contrast text color (black or white) for the given
 * "#RRGGBB" background color. Uses perceived luminance formula.
 * Returns LV_COLOR_WHITE for null, empty, or missing '#' prefix.
 */
inline lv_color_t GetContrastTextColor(const char* hexColor) {
  if (hexColor == nullptr || hexColor[0] != '#') return LV_COLOR_WHITE;
  const char* hex = hexColor + 1;
  uint32_t color = 0;
  for (int i = 0; hex[i] != '\0' && i < 6; i++) {
    char c = hex[i];
    uint8_t val = 0;
    if (c >= '0' && c <= '9') val = c - '0';
    else if (c >= 'a' && c <= 'f') val = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') val = c - 'A' + 10;
    color = (color << 4) | val;
  }
  uint8_t r = (color >> 16) & 0xFF;
  uint8_t g = (color >> 8) & 0xFF;
  uint8_t b = color & 0xFF;
  uint32_t luminance = (299 * r + 587 * g + 114 * b) / 1000;
  return (luminance > 128) ? LV_COLOR_BLACK : LV_COLOR_WHITE;
}

} // namespace HaBridge
} // namespace Applications
} // namespace Pinetime
