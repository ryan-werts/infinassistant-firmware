#pragma once

#include "displayapp/apps/Apps.h"
#include "displayapp/screens/Screen.h"
#include "displayapp/Controllers.h"
#include "components/ble/HomeAssistantService.h"
#include <lvgl/lvgl.h>

namespace Pinetime {
namespace Applications {
namespace Screens {

class HaLogbook : public Screen {
public:
  explicit HaLogbook(Controllers::HomeAssistantService& haService);
  ~HaLogbook() override;

  void Refresh() override;
  bool OnTouchEvent(TouchEvents event) override { return false; }

private:
  Controllers::HomeAssistantService& haService_;

  static constexpr uint8_t kMaxRows      = Controllers::HomeAssistantService::kMaxLogbookEntries;
  static constexpr uint8_t kRowHeight    = 26;
  static constexpr uint8_t kHeaderHeight = 30;

  lv_obj_t* container_   = nullptr;
  lv_obj_t* titleLabel_  = nullptr;
  lv_obj_t* emptyLabel_  = nullptr;
  lv_obj_t* rowLabels_[kMaxRows] = {};

  // Refresh() runs from this task: entries from the phone arrive while the screen is open.
  lv_task_t* taskRefresh_ = nullptr;
  uint8_t lastGeneration_ = 0;   // HomeAssistantService::GetLogbookGeneration() last drawn
  bool built_ = false;           // every object was created (HaCreated)

  void CreateUI();
  void UpdateRows();

  // Abbreviate state string: "CLOSED"→"CLSD", "OPENED"→"OPN", etc.
  static const char* AbbreviateState(const char* state);
  // The colour a state is drawn in: active states gold, inactive ones purple, others white.
  static uint32_t StateColor(const char* abbr);
};

} // namespace Screens

    template <>
    struct AppTraits<Apps::HaLogbook> {
      static constexpr Apps app = Apps::HaLogbook;
      static constexpr const char* icon = "L";

      static Screens::Screen* Create(AppControllers& controllers) {
        return new Screens::HaLogbook(*controllers.homeAssistantController);
      };

      static bool IsAvailable(Pinetime::Controllers::FS& /*filesystem*/) {
        return true;
      };
    };

} // namespace Applications
} // namespace Pinetime
