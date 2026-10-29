#pragma once

#include "components/ble/HomeAssistantService.h"
#include <lvgl/lvgl.h>

namespace Pinetime {
namespace Applications {
namespace Screens {

// FR-D5 — the HA app screens (calendar, home control, logbook, notifications) build
// their objects through this. lv_*_create returns null when the LVGL heap is out; the
// failure is counted (HELLO lvCreateFailures, as the HA face counts its own) and the
// screen stops building and stays as far as it got instead of dereferencing null.
inline lv_obj_t* HaCreated(lv_obj_t* obj, Controllers::HomeAssistantService& haService) {
  if (obj == nullptr) {
    haService.IncrementLvCreateFailures();
  }
  return obj;
}

// Text colours shared by the HA app screens, matching the HA face.
constexpr uint32_t kHaAppAccent = 0x18A1FF;    // titles
constexpr uint32_t kHaAppDim = 0x8A8A8A;       // times, hints, empty states
constexpr uint32_t kHaAppActive = 0xFFD700;    // on / open / unlocked
constexpr uint32_t kHaAppInactive = 0xB66DFF;  // off / closed / locked

} // namespace Screens
} // namespace Applications
} // namespace Pinetime
