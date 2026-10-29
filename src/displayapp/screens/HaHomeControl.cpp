#include "displayapp/screens/HaHomeControl.h"
#include "displayapp/screens/HaAppGuard.h"

using namespace Pinetime::Applications::Screens;

// The grid below is 2 x 2.
static_assert(Pinetime::Controllers::HomeAssistantService::kMaxHomeControlDevices == 4,
              "HaHomeControl lays out a 2x2 grid");

HaHomeControl::HaHomeControl(Controllers::HomeAssistantService& haService)
    : haService_(haService) {
  CreateUI();
  lastGeneration_ = haService_.GetHomeControlGeneration();
  UpdateTiles();
  taskRefresh_ = lv_task_create(RefreshTaskCallback, 250, LV_TASK_PRIO_MID, this);
}

HaHomeControl::~HaHomeControl() {
  if (taskRefresh_ != nullptr) {
    lv_task_del(taskRefresh_);
  }
  for (uint8_t i = 0; i < kMaxTiles; i++) {
    tileTapData_[i].screen = nullptr;
  }
  if (container_ != nullptr) {
    lv_obj_del(container_);
  }
}

void HaHomeControl::CreateUI() {
  container_ = HaCreated(lv_obj_create(lv_scr_act(), nullptr), haService_);
  if (container_ == nullptr) return;
  lv_obj_set_size(container_, 240, 240);
  lv_obj_set_pos(container_, 0, 0);
  lv_obj_set_style_local_bg_color(container_, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_BLACK);
  lv_obj_set_style_local_border_width(container_, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, 0);
  lv_obj_set_style_local_pad_all(container_, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, 0);

  // 2x2 grid positions: (col, row) each cell is kTileW x kTileH separated by kTileGap
  const uint8_t xs[kMaxTiles] = {
    kTileGap,
    static_cast<uint8_t>(kTileGap + kTileW + kTileGap),
    kTileGap,
    static_cast<uint8_t>(kTileGap + kTileW + kTileGap)
  };
  const uint8_t ys[kMaxTiles] = {
    kHeaderH,
    kHeaderH,
    static_cast<uint8_t>(kHeaderH + kTileH + kTileGap),
    static_cast<uint8_t>(kHeaderH + kTileH + kTileGap)
  };

  for (uint8_t i = 0; i < kMaxTiles; i++) {
    tiles_[i] = HaCreated(lv_btn_create(container_, nullptr), haService_);
    if (tiles_[i] == nullptr) return;
    lv_obj_set_size(tiles_[i], kTileW, kTileH);
    lv_obj_set_pos(tiles_[i], xs[i], ys[i]);
    lv_obj_set_style_local_radius(tiles_[i], LV_BTN_PART_MAIN, LV_STATE_DEFAULT, 10);
    lv_obj_set_style_local_bg_color(tiles_[i], LV_BTN_PART_MAIN, LV_STATE_DEFAULT, lv_color_hex(0x1A1A2E));
    lv_obj_set_style_local_border_width(tiles_[i], LV_BTN_PART_MAIN, LV_STATE_DEFAULT, 2);
    lv_obj_set_style_local_border_color(tiles_[i], LV_BTN_PART_MAIN, LV_STATE_DEFAULT, lv_color_hex(0x333355));
    lv_btn_set_layout(tiles_[i], LV_LAYOUT_OFF);

    // Device name, top of the tile.
    tileLabels_[i] = HaCreated(lv_label_create(tiles_[i], nullptr), haService_);
    if (tileLabels_[i] == nullptr) return;
    lv_label_set_long_mode(tileLabels_[i], LV_LABEL_LONG_BREAK);
    lv_obj_set_width(tileLabels_[i], kTileW - 12);
    lv_label_set_align(tileLabels_[i], LV_LABEL_ALIGN_CENTER);
    lv_obj_set_pos(tileLabels_[i], 6, 14);
    lv_obj_set_style_local_text_color(tileLabels_[i], LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_WHITE);
    lv_label_set_text_static(tileLabels_[i], "");

    // Current state, bottom of the tile.
    tileStateLabels_[i] = HaCreated(lv_label_create(tiles_[i], nullptr), haService_);
    if (tileStateLabels_[i] == nullptr) return;
    lv_label_set_long_mode(tileStateLabels_[i], LV_LABEL_LONG_CROP);
    lv_obj_set_width(tileStateLabels_[i], kTileW - 12);
    lv_label_set_align(tileStateLabels_[i], LV_LABEL_ALIGN_CENTER);
    lv_obj_set_pos(tileStateLabels_[i], 6, kTileH - 30);
    lv_obj_set_style_local_text_font(tileStateLabels_[i], LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, &lv_font_montserrat_14);
    lv_obj_set_style_local_text_color(tileStateLabels_[i], LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, lv_color_hex(kHaAppDim));
    lv_label_set_text_static(tileStateLabels_[i], "");

    tileTapData_[i] = {this, i};
    lv_obj_set_user_data(tiles_[i], &tileTapData_[i]);
    lv_obj_set_event_cb(tiles_[i], OnTileTapped);

    lv_obj_set_hidden(tiles_[i], true);
  }

  // Empty state label
  emptyLabel_ = HaCreated(lv_label_create(container_, nullptr), haService_);
  if (emptyLabel_ == nullptr) return;
  lv_label_set_text_static(emptyLabel_, "No devices");
  lv_obj_set_style_local_text_font(emptyLabel_, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, &lv_font_montserrat_14);
  lv_obj_set_style_local_text_color(emptyLabel_, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, lv_color_hex(kHaAppDim));
  lv_obj_align(emptyLabel_, container_, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_hidden(emptyLabel_, true);

  built_ = true;
}

void HaHomeControl::Refresh() {
  const uint8_t generation = haService_.GetHomeControlGeneration();
  if (generation == lastGeneration_) return;
  lastGeneration_ = generation;
  UpdateTiles();
}

void HaHomeControl::UpdateTiles() {
  if (!built_) return;
  const uint8_t count = haService_.GetHomeControlCount();
  const auto* devices = haService_.GetHomeControlDevices();

  for (uint8_t i = 0; i < kMaxTiles; i++) {
    if (devices == nullptr || i >= count) {
      lv_label_set_text_static(tileLabels_[i], "");
      lv_label_set_text_static(tileStateLabels_[i], "");
      lv_obj_set_hidden(tiles_[i], true);
      continue;
    }
    const auto& dev = devices[i];
    // lv_label_set_text copies into the label: the text lives only while the screen does.
    lv_label_set_text(tileLabels_[i], dev.label.data());
    lv_label_set_text(tileStateLabels_[i], dev.state.data());

    // Active: gold frame and state on a warm ground; inactive: the plain tile.
    lv_obj_set_style_local_bg_color(tiles_[i], LV_BTN_PART_MAIN, LV_STATE_DEFAULT,
                                    lv_color_hex(dev.active ? 0x3A3110 : 0x1A1A2E));
    lv_obj_set_style_local_border_color(tiles_[i], LV_BTN_PART_MAIN, LV_STATE_DEFAULT,
                                        lv_color_hex(dev.active ? kHaAppActive : 0x333355));
    lv_obj_set_style_local_text_color(tileStateLabels_[i], LV_LABEL_PART_MAIN, LV_STATE_DEFAULT,
                                      lv_color_hex(dev.active ? kHaAppActive : kHaAppDim));
    lv_obj_set_hidden(tiles_[i], false);
  }

  lv_obj_set_hidden(emptyLabel_, count > 0);
}

void HaHomeControl::OnTileTapped(lv_obj_t* obj, lv_event_t event) {
  if (event != LV_EVENT_CLICKED) return;
  auto* data = static_cast<TileTapData*>(lv_obj_get_user_data(obj));
  if (data == nullptr || data->screen == nullptr) return;
  auto& haService = data->screen->haService_;
  uint8_t count = haService.GetHomeControlCount();
  if (data->index >= count) return;
  const auto* devices = haService.GetHomeControlDevices();
  haService.SetPendingTileEntity(devices[data->index].entityId.data());
  // Not a watch-face tile: no result glyph to place (the index is left from the face's last tap).
  haService.SetPendingActionTileIndex(0xFF);
  haService.InvokeAction(0xFFFC, "tap");
}
