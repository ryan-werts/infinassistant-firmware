#pragma once

#include "displayapp/apps/Apps.h"
#include "displayapp/screens/Screen.h"
#include "displayapp/Controllers.h"
#include "components/ble/HomeAssistantService.h"
#include "ha_bridge/BridgeController.h"
#include <lvgl/lvgl.h>

namespace Pinetime {
namespace Applications {
namespace Screens {

class HaCalendar : public Screen {
public:
  HaCalendar(Controllers::HomeAssistantService& haService,
             HaBridge::BridgeController& bridgeController);
  ~HaCalendar() override;

  void Refresh() override;
  bool OnTouchEvent(TouchEvents event) override;

private:
  Controllers::HomeAssistantService& haService_;
  HaBridge::BridgeController& bridgeController_;

  static constexpr uint8_t kMaxRows     = Controllers::HomeAssistantService::kMaxCalendarEntries;
  static constexpr uint8_t kRowHeight   = 42;   // two lines of montserrat_14
  static constexpr uint8_t kHeaderHeight = 30;

  lv_obj_t* container_     = nullptr;
  lv_obj_t* dayLabel_      = nullptr;
  lv_obj_t* navLeftLabel_  = nullptr;
  lv_obj_t* navRightLabel_ = nullptr;
  lv_obj_t* emptyLabel_    = nullptr;
  lv_obj_t* rowLabels_[kMaxRows] = {};

  // Refresh() runs from this task: data from the phone arrives while the screen is open.
  lv_task_t* taskRefresh_ = nullptr;
  uint8_t lastGeneration_ = 0;   // HomeAssistantService::GetCalendarGeneration() last drawn
  bool built_ = false;           // every object was created (HaCreated)

  void CreateUI();
  void UpdateDisplay();
};

} // namespace Screens

    template <>
    struct AppTraits<Apps::HaCalendar> {
      static constexpr Apps app = Apps::HaCalendar;
      static constexpr const char* icon = "C";

      static Screens::Screen* Create(AppControllers& controllers) {
        return new Screens::HaCalendar(*controllers.homeAssistantController,
                                       *controllers.bridgeController);
      };

      static bool IsAvailable(Pinetime::Controllers::FS& /*filesystem*/) {
        return true;
      };
    };

} // namespace Applications
} // namespace Pinetime
