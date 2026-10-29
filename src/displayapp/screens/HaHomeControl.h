#pragma once

#include "displayapp/apps/Apps.h"
#include "displayapp/screens/Screen.h"
#include "displayapp/Controllers.h"
#include "components/ble/HomeAssistantService.h"
#include <lvgl/lvgl.h>

namespace Pinetime {
namespace Applications {
namespace Screens {

class HaHomeControl : public Screen {
public:
  explicit HaHomeControl(Controllers::HomeAssistantService& haService);
  ~HaHomeControl() override;

  void Refresh() override;
  bool OnTouchEvent(TouchEvents event) override { return false; }

private:
  Controllers::HomeAssistantService& haService_;

  static constexpr uint8_t kMaxTiles = Controllers::HomeAssistantService::kMaxHomeControlDevices;
  static constexpr uint8_t kTileW    = 116;
  static constexpr uint8_t kTileH    = 116;
  static constexpr uint8_t kTileGap  = 4;
  static constexpr uint8_t kHeaderH  = 4;

  lv_obj_t* container_             = nullptr;
  lv_obj_t* tiles_[kMaxTiles]      = {};
  lv_obj_t* tileLabels_[kMaxTiles]      = {};  // device name
  lv_obj_t* tileStateLabels_[kMaxTiles] = {};  // current state text
  lv_obj_t* emptyLabel_            = nullptr;

  struct TileTapData {
    HaHomeControl* screen;
    uint8_t index;
  };
  TileTapData tileTapData_[kMaxTiles] = {};

  // Refresh() runs from this task: device states from the phone arrive while the screen is open.
  lv_task_t* taskRefresh_ = nullptr;
  uint8_t lastGeneration_ = 0;   // HomeAssistantService::GetHomeControlGeneration() last drawn
  bool built_ = false;           // every object was created (HaCreated)

  void CreateUI();
  void UpdateTiles();
  static void OnTileTapped(lv_obj_t* obj, lv_event_t event);
};

} // namespace Screens

    template <>
    struct AppTraits<Apps::HaHomeControl> {
      static constexpr Apps app = Apps::HaHomeControl;
      static constexpr const char* icon = "H";

      static Screens::Screen* Create(AppControllers& controllers) {
        return new Screens::HaHomeControl(*controllers.homeAssistantController);
      };

      static bool IsAvailable(Pinetime::Controllers::FS& /*filesystem*/) {
        return true;
      };
    };

} // namespace Applications
} // namespace Pinetime
