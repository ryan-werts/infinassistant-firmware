#pragma once

#include <cstring>

namespace Pinetime {
namespace Applications {
namespace HaBridge {

/**
 * Check if an entity state string represents an "active" state for
 * color-coding purposes (e.g., gold for active, purple for inactive).
 *
 * Merged superset of all active states used across watch screens:
 *   on, open, home, playing, heat, cool, heat_cool, auto, unlocked, above_horizon
 */
inline bool IsEntityActive(const char* state) {
  if (state == nullptr || state[0] == '\0') return false;
  if (std::strcmp(state, "on") == 0) return true;
  if (std::strcmp(state, "open") == 0) return true;
  if (std::strcmp(state, "home") == 0) return true;
  if (std::strcmp(state, "playing") == 0) return true;
  if (std::strcmp(state, "heat") == 0) return true;
  if (std::strcmp(state, "cool") == 0) return true;
  if (std::strcmp(state, "heat_cool") == 0) return true;
  if (std::strcmp(state, "auto") == 0) return true;
  if (std::strcmp(state, "unlocked") == 0) return true;
  if (std::strcmp(state, "above_horizon") == 0) return true;
  return false;
}

} // namespace HaBridge
} // namespace Applications
} // namespace Pinetime
