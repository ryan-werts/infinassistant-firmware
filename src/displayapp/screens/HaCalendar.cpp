#include "displayapp/screens/HaCalendar.h"
#include "displayapp/screens/HaAppGuard.h"
#include <cstring>
#include <cstdio>

using namespace Pinetime::Applications::Screens;

HaCalendar::HaCalendar(Controllers::HomeAssistantService& haService,
                        HaBridge::BridgeController& bridgeController)
    : haService_(haService), bridgeController_(bridgeController) {
  CreateUI();
  // Opens on today, asking the phone for it: what the watch holds may be another day's, or old.
  haService_.SetCalendarDayOffset(0);
  bridgeController_.SendCalendarNav(0);
  lastGeneration_ = haService_.GetCalendarGeneration();
  UpdateDisplay();
  taskRefresh_ = lv_task_create(RefreshTaskCallback, 250, LV_TASK_PRIO_MID, this);
}

HaCalendar::~HaCalendar() {
  if (taskRefresh_ != nullptr) {
    lv_task_del(taskRefresh_);
  }
  if (container_ != nullptr) {
    lv_obj_del(container_);
  }
}

void HaCalendar::CreateUI() {
  container_ = HaCreated(lv_obj_create(lv_scr_act(), nullptr), haService_);
  if (container_ == nullptr) return;
  lv_obj_set_size(container_, 240, 240);
  lv_obj_set_pos(container_, 0, 0);
  lv_obj_set_style_local_bg_color(container_, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_BLACK);
  lv_obj_set_style_local_border_width(container_, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, 0);
  lv_obj_set_style_local_pad_all(container_, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, 0);

  // Header: "<  Today  >" — swipe right for the day before, left for the day after.
  navLeftLabel_ = HaCreated(lv_label_create(container_, nullptr), haService_);
  if (navLeftLabel_ == nullptr) return;
  lv_label_set_text_static(navLeftLabel_, "<");
  lv_obj_set_pos(navLeftLabel_, 6, 2);
  lv_obj_set_style_local_text_color(navLeftLabel_, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, lv_color_hex(kHaAppDim));

  dayLabel_ = HaCreated(lv_label_create(container_, nullptr), haService_);
  if (dayLabel_ == nullptr) return;
  lv_label_set_long_mode(dayLabel_, LV_LABEL_LONG_CROP);
  lv_obj_set_pos(dayLabel_, 24, 2);
  lv_obj_set_width(dayLabel_, 192);
  lv_label_set_align(dayLabel_, LV_LABEL_ALIGN_CENTER);
  lv_obj_set_style_local_text_color(dayLabel_, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, lv_color_hex(kHaAppAccent));
  lv_label_set_text_static(dayLabel_, "");

  navRightLabel_ = HaCreated(lv_label_create(container_, nullptr), haService_);
  if (navRightLabel_ == nullptr) return;
  lv_label_set_text_static(navRightLabel_, ">");
  lv_obj_set_pos(navRightLabel_, 222, 2);
  lv_obj_set_style_local_text_color(navRightLabel_, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, lv_color_hex(kHaAppDim));

  // One label per event, "time title" wrapping to two lines, the time in the accent colour.
  for (uint8_t i = 0; i < kMaxRows; i++) {
    rowLabels_[i] = HaCreated(lv_label_create(container_, nullptr), haService_);
    if (rowLabels_[i] == nullptr) return;
    lv_label_set_long_mode(rowLabels_[i], LV_LABEL_LONG_CROP);
    lv_obj_set_size(rowLabels_[i], 230, kRowHeight - 4);
    lv_obj_set_pos(rowLabels_[i], 6, kHeaderHeight + i * kRowHeight);
    lv_label_set_recolor(rowLabels_[i], true);
    lv_obj_set_style_local_text_font(rowLabels_[i], LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, &lv_font_montserrat_14);
    lv_obj_set_style_local_text_color(rowLabels_[i], LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_WHITE);
    lv_label_set_text_static(rowLabels_[i], "");
    lv_obj_set_hidden(rowLabels_[i], true);
  }

  emptyLabel_ = HaCreated(lv_label_create(container_, nullptr), haService_);
  if (emptyLabel_ == nullptr) return;
  lv_label_set_text_static(emptyLabel_, "No events");
  lv_obj_set_style_local_text_font(emptyLabel_, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, &lv_font_montserrat_14);
  lv_obj_set_style_local_text_color(emptyLabel_, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, lv_color_hex(kHaAppDim));
  lv_obj_align(emptyLabel_, container_, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_hidden(emptyLabel_, true);

  built_ = true;
}

void HaCalendar::Refresh() {
  const uint8_t generation = haService_.GetCalendarGeneration();
  if (generation == lastGeneration_) return;
  lastGeneration_ = generation;
  UpdateDisplay();
}

void HaCalendar::UpdateDisplay() {
  if (!built_) return;
  const int8_t offset  = haService_.GetCalendarDayOffset();
  const uint8_t count  = haService_.GetCalendarCount();
  const auto* entries  = haService_.GetCalendarEntries();

  if (offset == 0) {
    lv_label_set_text_static(dayLabel_, "Today");
  } else if (offset == 1) {
    lv_label_set_text_static(dayLabel_, "Tomorrow");
  } else if (offset == -1) {
    lv_label_set_text_static(dayLabel_, "Yesterday");
  } else {
    char day[12];
    std::snprintf(day, sizeof(day), "%+dd", static_cast<int>(offset));
    lv_label_set_text(dayLabel_, day);
  }

  for (uint8_t i = 0; i < kMaxRows; i++) {
    if (entries == nullptr || i >= count) {
      lv_label_set_text_static(rowLabels_[i], "");
      lv_obj_set_hidden(rowLabels_[i], true);
      continue;
    }
    // A '#' in the title would end the recolour span early.
    char title[sizeof(entries[i].title)];
    std::strncpy(title, entries[i].title.data(), sizeof(title) - 1);
    title[sizeof(title) - 1] = '\0';
    for (char* p = title; *p != '\0'; ++p) {
      if (*p == '#') *p = ' ';
    }
    char text[80];
    std::snprintf(text, sizeof(text), "#%06lx %s#  %s",
                  static_cast<unsigned long>(kHaAppAccent), entries[i].time.data(), title);
    // lv_label_set_text copies into the label: the text lives only while the screen does.
    lv_label_set_text(rowLabels_[i], text);
    lv_obj_set_hidden(rowLabels_[i], false);
  }
  lv_label_set_text_static(emptyLabel_, haService_.IsCalendarNavPending() ? "Loading..." : "No events");
  lv_obj_align(emptyLabel_, container_, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_hidden(emptyLabel_, count > 0);
}

bool HaCalendar::OnTouchEvent(TouchEvents event) {
  if (event == TouchEvents::SwipeRight) {
    // SwipeRight = go to previous day
    int8_t newOffset = static_cast<int8_t>(haService_.GetCalendarDayOffset() - 1);
    haService_.SetCalendarDayOffset(newOffset);
    bridgeController_.SendCalendarNav(newOffset);
    return true;
  }
  if (event == TouchEvents::SwipeLeft) {
    // SwipeLeft = go to next day
    int8_t newOffset = static_cast<int8_t>(haService_.GetCalendarDayOffset() + 1);
    haService_.SetCalendarDayOffset(newOffset);
    bridgeController_.SendCalendarNav(newOffset);
    return true;
  }
  return false;
}
