#include "displayapp/screens/HaLogbook.h"
#include "displayapp/screens/HaAppGuard.h"
#include <cstring>
#include <cstdio>

using namespace Pinetime::Applications::Screens;

HaLogbook::HaLogbook(Controllers::HomeAssistantService& haService)
    : haService_(haService) {
  CreateUI();
  lastGeneration_ = haService_.GetLogbookGeneration();
  UpdateRows();
  taskRefresh_ = lv_task_create(RefreshTaskCallback, 250, LV_TASK_PRIO_MID, this);
}

HaLogbook::~HaLogbook() {
  if (taskRefresh_ != nullptr) {
    lv_task_del(taskRefresh_);
  }
  if (container_ != nullptr) {
    lv_obj_del(container_);
  }
}

void HaLogbook::CreateUI() {
  container_ = HaCreated(lv_obj_create(lv_scr_act(), nullptr), haService_);
  if (container_ == nullptr) return;
  lv_obj_set_size(container_, 240, 240);
  lv_obj_set_pos(container_, 0, 0);
  lv_obj_set_style_local_bg_color(container_, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_BLACK);
  lv_obj_set_style_local_border_width(container_, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, 0);
  lv_obj_set_style_local_pad_all(container_, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, 0);

  titleLabel_ = HaCreated(lv_label_create(container_, nullptr), haService_);
  if (titleLabel_ == nullptr) return;
  lv_label_set_text_static(titleLabel_, "Logbook");
  lv_obj_set_pos(titleLabel_, 6, 2);
  lv_obj_set_style_local_text_color(titleLabel_, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, lv_color_hex(kHaAppAccent));

  // One label per entry, "time state subject", the time dim and the state coloured (recolour).
  // Pre-created and shown/hidden as data arrives (no create/destroy churn).
  for (uint8_t i = 0; i < kMaxRows; i++) {
    rowLabels_[i] = HaCreated(lv_label_create(container_, nullptr), haService_);
    if (rowLabels_[i] == nullptr) return;
    lv_obj_set_pos(rowLabels_[i], 6, kHeaderHeight + i * kRowHeight);
    lv_label_set_long_mode(rowLabels_[i], LV_LABEL_LONG_CROP);
    lv_obj_set_width(rowLabels_[i], 230);
    lv_label_set_recolor(rowLabels_[i], true);
    lv_obj_set_style_local_text_font(rowLabels_[i], LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, &lv_font_montserrat_14);
    lv_obj_set_style_local_text_color(rowLabels_[i], LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_WHITE);
    lv_label_set_text_static(rowLabels_[i], "");
    lv_obj_set_hidden(rowLabels_[i], true);
  }

  emptyLabel_ = HaCreated(lv_label_create(container_, nullptr), haService_);
  if (emptyLabel_ == nullptr) return;
  lv_label_set_text_static(emptyLabel_, "No entries yet");
  lv_obj_set_style_local_text_font(emptyLabel_, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, &lv_font_montserrat_14);
  lv_obj_set_style_local_text_color(emptyLabel_, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, lv_color_hex(kHaAppDim));
  lv_obj_align(emptyLabel_, container_, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_hidden(emptyLabel_, true);

  built_ = true;
}

void HaLogbook::Refresh() {
  const uint8_t generation = haService_.GetLogbookGeneration();
  if (generation == lastGeneration_) return;
  lastGeneration_ = generation;
  UpdateRows();
}

void HaLogbook::UpdateRows() {
  if (!built_) return;
  const uint8_t count = haService_.GetLogbookCount();
  const auto* entries = haService_.GetLogbookEntries();

  for (uint8_t i = 0; i < kMaxRows; i++) {
    if (entries == nullptr || i >= count) {
      lv_label_set_text_static(rowLabels_[i], "");
      lv_obj_set_hidden(rowLabels_[i], true);
      continue;
    }
    const auto& e = entries[i];
    const char* abbr = AbbreviateState(e.state.data());
    // A '#' in the subject would end the recolour span early.
    char subject[sizeof(e.subject)];
    std::strncpy(subject, e.subject.data(), sizeof(subject) - 1);
    subject[sizeof(subject) - 1] = '\0';
    for (char* p = subject; *p != '\0'; ++p) {
      if (*p == '#') *p = ' ';
    }
    char text[96];
    std::snprintf(text, sizeof(text), "#%06lx %s#  #%06lx %s#  %s",
                  static_cast<unsigned long>(kHaAppDim), e.time.data(),
                  static_cast<unsigned long>(StateColor(abbr)), abbr, subject);
    // lv_label_set_text copies into the label: the text lives only while the screen does.
    lv_label_set_text(rowLabels_[i], text);
    lv_obj_set_hidden(rowLabels_[i], false);
  }

  lv_obj_set_hidden(emptyLabel_, count > 0);
}

const char* HaLogbook::AbbreviateState(const char* state) {
  if (state == nullptr || state[0] == '\0') return "?";
  if (std::strcmp(state, "CLOSED")      == 0) return "CLSD";
  if (std::strcmp(state, "OPENED")      == 0) return "OPN";
  if (std::strcmp(state, "LOCKED")      == 0) return "LCKD";
  if (std::strcmp(state, "UNLOCKED")    == 0) return "ULKD";
  if (std::strcmp(state, "TRIGGERED")   == 0) return "TRGD";
  if (std::strcmp(state, "UNAVAILABLE") == 0) return "N/A";
  return state;  // ON, OFF, etc. — short enough as-is
}

uint32_t HaLogbook::StateColor(const char* abbr) {
  static constexpr const char* kActive[] = {"ON", "OPN", "ULKD", "TRGD", "HOME", "PLAY"};
  static constexpr const char* kInactive[] = {"OFF", "CLSD", "LCKD", "CLR", "AWAY", "IDLE", "PAUS", "STBY"};
  for (const char* s : kActive) {
    if (std::strcmp(abbr, s) == 0) return kHaAppActive;
  }
  for (const char* s : kInactive) {
    if (std::strcmp(abbr, s) == 0) return kHaAppInactive;
  }
  return 0xFFFFFF;
}
