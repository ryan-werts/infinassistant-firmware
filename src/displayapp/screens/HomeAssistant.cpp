#include "displayapp/screens/HomeAssistant.h"
#include "displayapp/screens/icons/tile_icons.h"
#include "components/battery/BatteryController.h"
#include "components/heartrate/HeartRateController.h"
#include "components/motion/MotionController.h"
#include "ha_bridge/LayoutStoreFlash.h"
#include "ha_bridge/BootConfirm.h"
#include "ha_bridge/BridgeController.h"  // REQ-060: WatchdogSubsystemKick + UpdateDisplayStackHwm
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <FreeRTOS.h>

using namespace Pinetime::Applications::Screens;

// The montserrat fonts used below are declared by LVGL itself: lv_conf.h sets
// LV_FONT_MONTSERRAT_14/18 to 1, so lv_font.h's LV_FONT_DECLARE macros fire.
//
// This used to carry its own `extern const lv_font_t` declarations, with a
// comment claiming the lv_conf.h flags were 0. They are 1, so the declarations
// were redundant — and actively harmful off-device: LV_FONT_DECLARE is
// const-qualified in this fork but NOT in stock LVGL, so re-declaring them
// const broke the simulator build with a conflicting declaration. Rely on
// LVGL's declaration and let each tree use its own qualification.

// C2 (memory hardening): out-of-line definitions for the STATIC shared styles.
// Initialized exactly once in CreateSharedWidgets() and NEVER lv_style_reset — they
// persist for the process lifetime so navigate-away/back does not churn/fragment the
// heap. Reused by each reconstruction's lv_obj_add_style().
lv_style_t HomeAssistant::haStyleTime_;
lv_style_t HomeAssistant::haStyleStatusBold_;
lv_style_t HomeAssistant::haStyleScreenInd_;
lv_style_t HomeAssistant::haStyleTileChrome_;
lv_style_t HomeAssistant::haStyleContainerChrome_;
lv_style_t HomeAssistant::haStyleDashPill_[HaBridge::DashV8::kBands + 1];
lv_style_t HomeAssistant::haStyleDashBar_;
lv_style_t HomeAssistant::haStyleDashInd_;
bool HomeAssistant::haStylesInitialized_ = false;
Pinetime::Applications::HaBridge::DashLive HomeAssistant::dashLiveCopy_;

// REQ-001 (HA_GLYPH_PROBE) — single-label diagnostic build that replaces the
// HA constructor body and logs lv_mem_monitor + font/glyph descriptors. Off by
// default; enable via -DHA_GLYPH_PROBE=1 at compile time.
#ifndef HA_GLYPH_PROBE
#define HA_GLYPH_PROBE 0
#endif

// REQ-005 (HA_LV_TELEMETRY) — instrument LVGL pool snapshot at entry of each
// HA Create*/Configure* boundary. Off by default; enable via -DHA_LV_TELEMETRY=1.
// Writes one line per checkpoint into g_haProbeLog, format:
//   HA_LV: stage=<name> free=N biggest=N frag=N
#ifndef HA_LV_TELEMETRY
#define HA_LV_TELEMETRY 0
#endif

#if HA_LV_TELEMETRY
#define HA_LV_LOG(stage) HaLvLog(stage)
#else
#define HA_LV_LOG(stage) ((void)0)
#endif

// REQ-006 — three-build discriminator. Compile with exactly one of the
// following set to 1 to test a specific hypothesis. FULL is the default.
//   HA_BUILD_VARIANT_DEFAULT_ONLY  — HA constructor only creates the default
//      group (no tiles, no dashboard). Tests whether the issue is in tile or
//      dashboard creation paths vs the default help-text path.
//   HA_BUILD_VARIANT_FROZEN_DASH   — HA renders the dashboard from a fixed
//      compiled-in fixture; BLE WATCHFACE_LAYOUT handler returns early without
//      mutating live layout. Tests whether the issue is the live BLE push race
//      vs the dashboard render itself.
//   HA_BUILD_VARIANT_FULL          — normal pipeline. Default.
#ifndef HA_BUILD_VARIANT_DEFAULT_ONLY
#define HA_BUILD_VARIANT_DEFAULT_ONLY 0
#endif
#ifndef HA_BUILD_VARIANT_FROZEN_DASH
#define HA_BUILD_VARIANT_FROZEN_DASH 0
#endif
#ifndef HA_BUILD_VARIANT_FULL
#define HA_BUILD_VARIANT_FULL 1
#endif
static_assert((HA_BUILD_VARIANT_DEFAULT_ONLY + HA_BUILD_VARIANT_FROZEN_DASH) <= 1,
              "REQ-006: at most one HA_BUILD_VARIANT_* may be set to 1");

namespace {
  constexpr int SCREEN_WIDTH = 240;
  constexpr int SCREEN_HEIGHT = 240;

  // Tile screens: a 2 x 2 grid (see "Tile screens" below).
  constexpr lv_coord_t kTileMargin = 4;
  constexpr lv_coord_t kTileGap = 4;
  constexpr lv_coord_t kTileW = (SCREEN_WIDTH - 2 * kTileMargin - kTileGap) / 2;  // 114
  constexpr lv_coord_t kTileH = 104;     // the rows end at y = 216, above the indicator's digits
  constexpr lv_coord_t kTileRadius = 10;
  constexpr lv_coord_t kTileFrameWidth = 2;
  constexpr lv_coord_t kTilePadX = 4;    // text inset from the frame
  constexpr lv_coord_t kTileTextW = kTileW - 2 * kTilePadX;
}

// Shared probe log buffer — readable via external symbol by HELLO encoder or
// serial dump. 256 B is enough for one probe payload; callers must respect
// remaining capacity (HaProbeLogAppend clamps).
char g_haProbeLog[256] = {0};

namespace {
  // Append-formatted helper for the probe log. Never advances past the buffer end.
  void HaProbeLogAppend(const char* fmt, ...) {
    const size_t cap = sizeof(g_haProbeLog);
    // g_haProbeLog is zero-initialized at startup and only ever extended by
    // vsnprintf below (which always null-terminates), so strlen is safe.
    size_t pos = strlen(g_haProbeLog);
    if (pos + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_haProbeLog + pos, cap - pos, fmt, ap);
    va_end(ap);
  }

  // REQ-003 — bounded snprintf-append helper. Returns the new pos clamped to
  // [0, bufSize-1] so subsequent buf[pos] = '\0' is always in-range. Replaces
  // the `pos += snprintf(...)` pattern that would write past the buffer when
  // snprintf returned "would-have-written" >= remaining capacity.
  int AppendFmt(char* buf, size_t bufSize, int pos, const char* fmt, ...) {
    if (buf == nullptr || bufSize == 0) return 0;
    if (pos < 0) pos = 0;
    if (static_cast<size_t>(pos) >= bufSize) {
      buf[bufSize - 1] = '\0';
      return static_cast<int>(bufSize - 1);
    }
    const size_t remaining = bufSize - static_cast<size_t>(pos);
    va_list ap;
    va_start(ap, fmt);
    const int wrote = vsnprintf(buf + pos, remaining, fmt, ap);
    va_end(ap);
    if (wrote < 0) {
      buf[pos] = '\0';
      return pos;
    }
    // vsnprintf returns "would-have-written" which can exceed remaining-1.
    const int newPos = pos + ((static_cast<size_t>(wrote) >= remaining)
                                 ? static_cast<int>(remaining - 1)
                                 : wrote);
    if (static_cast<size_t>(newPos) >= bufSize) {
      buf[bufSize - 1] = '\0';
      return static_cast<int>(bufSize - 1);
    }
    buf[newPos] = '\0';
    return newPos;
  }

#if HA_LV_TELEMETRY
  // REQ-005 — append one HA_LV: line per checkpoint to the probe log.
  void HaLvLog(const char* stage) {
    lv_mem_monitor_t mon = {};
    lv_mem_monitor(&mon);
    HaProbeLogAppend("HA_LV: stage=%s free=%u biggest=%u frag=%u\n",
                     stage,
                     (unsigned)mon.free_size,
                     (unsigned)mon.free_biggest_size,
                     (unsigned)mon.frag_pct);
  }
#endif
}

// Static constexpr member definitions (C++14 requires this for ODR-use)
constexpr char HomeAssistant::kDefaultHelpText_[];
constexpr char HomeAssistant::kNoDataHelpText_[];

// REQ-030 — LV object create wrapper. Forwards through to haService_'s
// failure counter when lv_*_create returns null. Hot-path inlinable.
lv_obj_t* HomeAssistant::LvObjOrNull(lv_obj_t* obj) {
  if (obj == nullptr) {
    haService_.IncrementLvCreateFailures();
  }
  return obj;
}

// REQ-064 — Evaluate safe-face trip conditions and update safeFaceMode_.
// Called once at construction (before ShowScreen) and every Refresh() tick so
// the face recovers automatically when memory pressure eases.
//
// Trip conditions (OR):
//   1. lv_mem_monitor().free_biggest_size < kLvFreeBiggestSafeThresholdBytes (1 KB)
//   2. lv_*_create() failures since this face was constructed (per-activation
//      baseline over haService_.GetLvCreateFailures(), which stays cumulative)
//
// On false→true transition: increments haService_.safeFaceTrips_ (exposed via
// HELLO field "safeFaceTrips", field 26 / base; 27 with crashContext).
// On true→false: clears the flag silently — the trip counter is never decremented.
void HomeAssistant::CheckSafeFaceCondition() {
  // Use the REAL FreeRTOS free-heap figure. lv_mem_monitor() is useless here:
  // under LV_MEM_CUSTOM=1 LVGL allocates via pvPortMalloc and never populates
  // its monitor, so free_biggest_size is always 0 and the old check tripped the
  // guard on every tick (permanent "layout too heavy"). xPortGetFreeHeapSize()
  // is the same source the HELLO diagnostic reports as freeHeap.
  const uint32_t freeHeap = static_cast<uint32_t>(xPortGetFreeHeapSize());
  const bool lowHeapTrip = (freeHeap < kFreeHeapSafeThresholdBytes);
  // Per-activation: failures since this face was constructed. The service counter is
  // cumulative (HELLO diag), so an old failure does not trip a fresh face.
  const bool createTrip = (static_cast<uint16_t>(haService_.GetLvCreateFailures() - lvCreateBaseline_) > 0);
  const bool shouldBeActive = lowHeapTrip || createTrip;

  if (shouldBeActive && !safeFaceMode_) {
    // false → true transition: count the trip
    haService_.IncrementSafeFaceTrips();
  }
  safeFaceMode_ = shouldBeActive;
}

HomeAssistant::HomeAssistant(Controllers::DateTime& dateTimeController,
                             const Controllers::Battery& batteryController,
                             Controllers::HeartRateController& heartRateController,
                             Controllers::MotionController& motionController,
                             Controllers::HomeAssistantService& haService)
    : Screen(),
      dateTimeController_(dateTimeController),
      batteryController_(batteryController),
      heartRateController_(heartRateController),
      motionController_(motionController),
      haService_(haService),
      model_(*this) {
#if HA_GLYPH_PROBE
  // REQ-001 — probe-only ctor: build one styled label and log LVGL pool stats +
  // font/glyph descriptors, then early-return. Used to discriminate font/linker
  // corruption (probe garbles) from LVGL pool/style pressure (probe clean).
  container_ = lv_scr_act();
  lv_obj_set_style_local_bg_color(container_, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_BLACK);
  lv_obj_set_style_local_bg_opa(container_, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, LV_OPA_COVER);

  lv_obj_t* probeLabel = LvObjOrNull(lv_label_create(container_, nullptr));
  if (probeLabel) {
    // Probe path runs before CreateSharedWidgets() inits shared styles, so
    // keep the local style here — probe builds never enter the normal path.
    lv_obj_set_style_local_text_font(probeLabel, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, &jetbrains_mono_42);
    lv_obj_set_style_local_text_color(probeLabel, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_WHITE);
    lv_label_set_text_static(probeLabel, "012345");
    lv_obj_align(probeLabel, container_, LV_ALIGN_CENTER, 0, 0);
  }

  lv_mem_monitor_t mon = {};
  lv_mem_monitor(&mon);

  const lv_font_t* font = &jetbrains_mono_42;
  lv_font_glyph_dsc_t gd = {};
  const bool gok = lv_font_get_glyph_dsc(font, &gd, '0', 0);
  const uint8_t* bmp = lv_font_get_glyph_bitmap(font, '0');

  g_haProbeLog[0] = '\0';
  HaProbeLogAppend("HA_PROBE: free=%u biggest=%u frag=%u glyph_ok=%d "
                   "bitmap=0x%08lx adv_w=%u box_w=%u box_h=%u",
                   (unsigned)mon.free_size,
                   (unsigned)mon.free_biggest_size,
                   (unsigned)mon.frag_pct,
                   (int)gok,
                   (unsigned long)(uintptr_t)bmp,
                   (unsigned)gd.adv_w,
                   (unsigned)gd.box_w,
                   (unsigned)gd.box_h);
  return;  // Bypass normal construction; refresh task NOT created.
#endif
#if HA_BOOT_TRACE
  HaBridge::BootConfirm::MarkStep(1);
#endif
  lvCreateBaseline_ = haService_.GetLvCreateFailures();
  // Activation is a materialization: the model takes the heap phase, reads the store and
  // returns what to draw, with the phase still held. Present() then builds the shared
  // widgets, draws and ends the hold, so a transfer's first chunk is refused throughout.
  // A deferred first draw builds NOTHING here: Refresh() builds everything once the phase
  // can be taken.
  //
  // The boot case: this constructor runs inside LoadNewScreen, in the window where an
  // unconfirmed image must reach its first frame, and 2.4.62 reset here while reading the
  // store (littlefs, on the flash). The first face of an unconfirmed boot therefore takes the
  // boot-confirmation hold and activates WITHOUT reading: it owes the read, which the first
  // Refresh() serves on the next tick, outside LoadNewScreen. The confirmation then waits
  // for that read to be drawn (Present), so a failure in it still reverts the image. Every
  // other construction (a confirmed image, a later face) reads here as before.
  bootReadHeld_ = HaBridge::BootConfirm::HoldForFirstRead();
  Present(bootReadHeld_ ? model_.ActivateDeferred(0) : model_.Activate(0));
  taskRefresh_ = lv_task_create([](lv_task_t* task) {
    auto* screen = static_cast<HomeAssistant*>(task->user_data);
    screen->Refresh();
  }, 1000, LV_TASK_PRIO_MID, this);
#if HA_BOOT_TRACE
  HaBridge::BootConfirm::MarkStep(2);
#endif
}

HomeAssistant::~HomeAssistant() {
  // 1. Delete refresh task FIRST (prevents callbacks during cleanup)
  if (taskRefresh_ != nullptr) {
    lv_task_del(taskRefresh_);
    taskRefresh_ = nullptr;
  }

  // 1a. A face destroyed before its first read was drawn must not hold the boot
  // confirmation for good.
  if (bootReadHeld_) {
    bootReadHeld_ = false;
    HaBridge::BootConfirm::ReleaseFirstRead();
  }

  // 1b. Free the view block (no LVGL object; the display task frees it).
  model_.Deactivate();

  // 2. Invalidate tile tap data pointers
  for (uint8_t i = 0; i < tileTapCount_; ++i) {
    tileTapPool_[i].haService = nullptr;
  }
  tileTapCount_ = 0;

  // 3. Clean all children of screen — standard InfiniTime watch face pattern
  lv_obj_clean(lv_scr_act());

  // 4. Null out all pointers (now dangling)
  container_ = nullptr;
  timeLabel_ = nullptr;
  statusLabel_ = nullptr;
  screenIndicatorLabel_ = nullptr;
  staleBanner_ = nullptr;  // REQ-065
  nextEventLabel_ = nullptr;  // REQ-070
  defaultHelpLabel_ = nullptr;
  for (uint8_t i = 0; i < kMaxTiles; i++) {
    tileObjs_[i] = nullptr;
  }
  for (auto& obj : dash_) {
    obj = nullptr;
  }

  // C2 (memory hardening): shared styles are STATIC + init-once + NEVER reset here.
  // Resetting + re-initializing them on every navigation away/back fragmented the
  // ~24 KB heap and was a primary cause of the LVGL-OOM NULL-deref hardfault on HA
  // reconstruction. They persist for the process lifetime (a few small, stable
  // allocations) and are reused by the next construction's lv_obj_add_style().
  // (haStylesInitialized_ stays true so CreateSharedWidgets() skips re-init.)
}

// ============================================================================
// LVGL allocation strategy (nRF52832, heap_4, ~3-5KB free heap after BLE/services)
//
// Create only 3 shared labels at boot (time/status/indicator).
// Lazily create heavier groups (tiles, buttons, help text, logbook rows)
// the first time they are actually shown.
// Never destroy widgets; only hide/show them — no alloc/free churn on swipes.
// ============================================================================
void HomeAssistant::CreateSharedWidgets() {
  HA_LV_LOG("CreateSharedWidgets");  // REQ-005
  // REQ-004 — initialize shared style registry once. Subsequent widget
  // creates lv_obj_add_style() these instead of issuing per-widget
  // property overrides. Net effect on LVGL pool: 45+ per-widget style
  // allocations collapse into 8 shared style allocations.
  if (!haStylesInitialized_) {
    lv_style_init(&haStyleTime_);
    // Time font: jetbrains_mono_bold_20 by default (the tile screens drew it between their
    // rows until 2.4.94). The dashboard and the default face draw the clock in
    // jetbrains_mono_42 (SetClockFont).
    // 2.4.26 had cut every clock to bold_20 because the 42 px digits garbled after
    // navigation; that garble was LVGL writing its glyph cache into the const font
    // descriptors in flash, fixed by the font guard in 2.4.87, not heap pressure.
    lv_style_set_text_font(&haStyleTime_, LV_STATE_DEFAULT, &jetbrains_mono_bold_20);
    lv_style_set_text_color(&haStyleTime_, LV_STATE_DEFAULT, LV_COLOR_WHITE);

    lv_style_init(&haStyleStatusBold_);
    lv_style_set_text_font(&haStyleStatusBold_, LV_STATE_DEFAULT, &jetbrains_mono_bold_20);
    lv_style_set_text_color(&haStyleStatusBold_, LV_STATE_DEFAULT, LV_COLOR_MAKE(0x99, 0x99, 0x99));

    lv_style_init(&haStyleScreenInd_);
    lv_style_set_text_color(&haStyleScreenInd_, LV_STATE_DEFAULT, LV_COLOR_MAKE(0x60, 0x60, 0x60));

    // The dashboard's (v8). A pill is one label carrying one of these: its band's fill or grey
    // (stale), a black letter; recolouring it swaps the style (SwapStyle), nothing is allocated.
    for (uint8_t b = 0; b <= HaBridge::DashV8::kBands; b++) {
      lv_style_t* pill = &haStyleDashPill_[b];
      lv_style_init(pill);
      lv_style_set_bg_opa(pill, LV_STATE_DEFAULT, LV_OPA_COVER);
      lv_style_set_bg_color(pill, LV_STATE_DEFAULT,
                            lv_color_hex(HaBridge::DashV8::PillFill(b, b == HaBridge::DashV8::kBands)));
      lv_style_set_radius(pill, LV_STATE_DEFAULT, 8);
      lv_style_set_text_font(pill, LV_STATE_DEFAULT, &lv_font_montserrat_14);
      lv_style_set_text_color(pill, LV_STATE_DEFAULT, LV_COLOR_BLACK);
    }

    // The freezer bar and its text in one label. The text's line box starts 3 px above the bar
    // so its baseline sits at y 191, as drawn; the glyphs themselves stay inside the bar.
    lv_style_init(&haStyleDashBar_);
    lv_style_set_bg_opa(&haStyleDashBar_, LV_STATE_DEFAULT, LV_OPA_COVER);
    lv_style_set_bg_color(&haStyleDashBar_, LV_STATE_DEFAULT, lv_color_hex(HaBridge::DashV8::kAlarm));
    lv_style_set_pad_top(&haStyleDashBar_, LV_STATE_DEFAULT, -3);
    lv_style_set_text_font(&haStyleDashBar_, LV_STATE_DEFAULT, &jetbrains_mono_bold_20);
    lv_style_set_text_color(&haStyleDashBar_, LV_STATE_DEFAULT, LV_COLOR_BLACK);

    // The screen indicator on the dashboard, in place of haStyleScreenInd_ (PlaceIndicator).
    lv_style_init(&haStyleDashInd_);
    lv_style_set_text_font(&haStyleDashInd_, LV_STATE_DEFAULT, &lv_font_montserrat_14);
    lv_style_set_text_color(&haStyleDashInd_, LV_STATE_DEFAULT, lv_color_hex(HaBridge::DashV8::kFaint));

    // The tile screens' tiles, framed as the Home Control app's: RenderTile sets the ground
    // and frame colours.
    lv_style_init(&haStyleTileChrome_);
    lv_style_set_bg_opa(&haStyleTileChrome_, LV_STATE_DEFAULT, LV_OPA_COVER);
    lv_style_set_radius(&haStyleTileChrome_, LV_STATE_DEFAULT, kTileRadius);
    lv_style_set_border_width(&haStyleTileChrome_, LV_STATE_DEFAULT, kTileFrameWidth);
    lv_style_set_pad_left(&haStyleTileChrome_, LV_STATE_DEFAULT, kTilePadX);
    lv_style_set_pad_right(&haStyleTileChrome_, LV_STATE_DEFAULT, kTilePadX);
    lv_style_set_text_font(&haStyleTileChrome_, LV_STATE_DEFAULT, &jetbrains_mono_bold_20);
    lv_style_set_text_color(&haStyleTileChrome_, LV_STATE_DEFAULT, LV_COLOR_WHITE);

    lv_style_init(&haStyleContainerChrome_);
    lv_style_set_bg_color(&haStyleContainerChrome_, LV_STATE_DEFAULT, LV_COLOR_BLACK);
    lv_style_set_bg_opa(&haStyleContainerChrome_, LV_STATE_DEFAULT, LV_OPA_COVER);
    lv_style_set_border_width(&haStyleContainerChrome_, LV_STATE_DEFAULT, 0);
    lv_style_set_radius(&haStyleContainerChrome_, LV_STATE_DEFAULT, 0);
    lv_style_set_pad_all(&haStyleContainerChrome_, LV_STATE_DEFAULT, 0);

    haStylesInitialized_ = true;
  }
  // REQ-029 — apply all background/chrome styles to a dedicated child
  // container instead of lv_scr_act(). Without this, the bg_color/bg_opa
  // overrides persist on the root after lv_obj_clean(lv_scr_act()) in the
  // dtor (which only removes children, not the root's own local style
  // overrides), bleeding HA's black background into the next screen.
  // Destroying container_ via the dtor's lv_obj_clean implicitly drops
  // its style allocations — equivalent to an explicit _reset on lv_scr_act
  // with one fewer call and no LVGL pool churn.
  container_ = LvObjOrNull(lv_obj_create(lv_scr_act(), nullptr));
  if (container_ != nullptr) {
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(container_, 0, 0);
    lv_obj_add_style(container_, LV_OBJ_PART_MAIN, &haStyleContainerChrome_);
  } else {
    container_ = lv_scr_act();  // Fallback: still functional, but leaks styles on dtor.
  }

  // --- Shared labels (used by all screen types) ---
  timeLabel_ = LvObjOrNull(lv_label_create(container_, nullptr));
  if (timeLabel_) {
    lv_obj_add_style(timeLabel_, LV_LABEL_PART_MAIN, &haStyleTime_);
    lv_label_set_text_static(timeLabel_, "");
    lv_obj_set_hidden(timeLabel_, true);
  }

  statusLabel_ = LvObjOrNull(lv_label_create(container_, nullptr));
  if (statusLabel_) {
    lv_obj_add_style(statusLabel_, LV_LABEL_PART_MAIN, &haStyleStatusBold_);
    lv_label_set_recolor(statusLabel_, true);  // the charging plug is green (UpdateTimeAndStatus)
    lv_label_set_text_static(statusLabel_, "");
    lv_obj_set_hidden(statusLabel_, true);
  }

  screenIndicatorLabel_ = LvObjOrNull(lv_label_create(container_, nullptr));
  if (screenIndicatorLabel_) {
    lv_obj_add_style(screenIndicatorLabel_, LV_LABEL_PART_MAIN, &haStyleScreenInd_);
    lv_label_set_text_static(screenIndicatorLabel_, "");
    lv_obj_set_hidden(screenIndicatorLabel_, true);
  }

  // REQ-065 — stale-data banner: fixed-height strip at the bottom of the
  // screen, overlaid on every group. Created hidden; UpdateTimeAndStatus()
  // shows/hides it based on IsDataStale() + lastEnrichedAt age.
  staleBanner_ = LvObjOrNull(lv_label_create(container_, nullptr));
  if (staleBanner_) {
    lv_obj_set_style_local_bg_opa(staleBanner_, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, LV_OPA_COVER);
    lv_obj_set_style_local_bg_color(staleBanner_, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT,
                                    LV_COLOR_MAKE(0xCC, 0x00, 0x00));
    lv_obj_set_style_local_text_color(staleBanner_, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT,
                                      LV_COLOR_WHITE);
    lv_obj_set_style_local_text_font(staleBanner_, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT,
                                     &lv_font_montserrat_14);
    lv_obj_set_style_local_pad_top(staleBanner_, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, 1);
    lv_obj_set_style_local_pad_bottom(staleBanner_, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, 1);
    lv_label_set_align(staleBanner_, LV_LABEL_ALIGN_CENTER);
    lv_label_set_text_static(staleBanner_, "");
    // Pin to bottom of screen; width = full 240px, height = auto (font height + pad).
    lv_obj_set_width(staleBanner_, 240);
    lv_obj_align(staleBanner_, container_, LV_ALIGN_IN_BOTTOM_MID, 0, 0);
    lv_obj_set_hidden(staleBanner_, true);
  }

  // REQ-070 — "next event in Xm" header: thin strip at the top of the screen,
  // overlaid on every group. Shown when a calendar event starts within 30 min.
  nextEventLabel_ = LvObjOrNull(lv_label_create(container_, nullptr));
  if (nextEventLabel_) {
    lv_obj_set_style_local_bg_opa(nextEventLabel_, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, LV_OPA_COVER);
    lv_obj_set_style_local_bg_color(nextEventLabel_, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT,
                                    LV_COLOR_MAKE(0x00, 0x60, 0xA0));
    lv_obj_set_style_local_text_color(nextEventLabel_, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT,
                                      LV_COLOR_WHITE);
    lv_obj_set_style_local_text_font(nextEventLabel_, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT,
                                     &lv_font_montserrat_14);
    lv_obj_set_style_local_pad_top(nextEventLabel_, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, 1);
    lv_obj_set_style_local_pad_bottom(nextEventLabel_, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, 1);
    lv_label_set_align(nextEventLabel_, LV_LABEL_ALIGN_CENTER);
    lv_label_set_text_static(nextEventLabel_, "");
    lv_obj_set_width(nextEventLabel_, 240);
    lv_obj_align(nextEventLabel_, container_, LV_ALIGN_IN_TOP_MID, 0, 0);
    lv_obj_set_hidden(nextEventLabel_, true);
  }
}

// ============================================================================
// Batch group creation — allocate ALL widgets for a group in one burst,
// guarded by a bool flag so each group is created at most once.
// Configure* methods then only reposition/restyle — zero allocations.
// ============================================================================

void HomeAssistant::CreateDefaultGroupOnce() {
  if (defaultGroupCreated_) return;
  HA_LV_LOG("CreateDefaultGroupOnce");  // REQ-005
  defaultGroupCreated_ = true;

  defaultHelpLabel_ = LvObjOrNull(lv_label_create(container_, nullptr));
  if (defaultHelpLabel_) {
    lv_obj_add_style(defaultHelpLabel_, LV_LABEL_PART_MAIN, &haStyleScreenInd_);
    lv_label_set_text_static(defaultHelpLabel_, kDefaultHelpText_);
    lv_obj_set_hidden(defaultHelpLabel_, true);
  }
}

void HomeAssistant::CreateTileGroupOnce() {
  if (tileGroupCreated_) return;
  HA_LV_LOG("CreateTileGroupOnce");  // REQ-005
  tileGroupCreated_ = true;

  // Create ALL tile labels BARE — no styles, no font, no click.
  // Styles are applied in ConfigureTileGroup() only for visible tiles.
  // This reduces peak heap: bare lv_label ~80-100 bytes vs styled ~200-250 bytes.
  for (uint8_t i = 0; i < kMaxTiles; i++) {
    lv_obj_t* tile = LvObjOrNull(lv_label_create(container_, nullptr));
    if (tile == nullptr) continue;
    lv_label_set_long_mode(tile, LV_LABEL_LONG_CROP);
    lv_label_set_align(tile, LV_LABEL_ALIGN_CENTER);
    lv_label_set_text_static(tile, "");
    lv_obj_set_hidden(tile, true);
    tileObjs_[i] = tile;
  }

}

void HomeAssistant::CreateDashboardGroupOnce() {
  if (dashboardGroupCreated_) return;
  HA_LV_LOG("CreateDashboardGroupOnce");  // REQ-005
  dashboardGroupCreated_ = true;

  // v8 (2.4.98): the element table of docs/face_dashboard/v8/design-spec.md in thirteen objects,
  // created hidden and placed once (the clock and the indicator are the shared labels). The
  // plain texts and the rule are drawn by one object, the panel (DrawDashPanel, kDashBoxes):
  // a label each would cost about 136 B of heap apiece. The pills, the check and the freezer
  // bar are labels, each with one shared style.
  dash_[DashPanel] = LvObjOrNull(lv_obj_create(container_, nullptr));
  if (dash_[DashPanel]) {
    lv_obj_t* panel = dash_[DashPanel];
    lv_obj_set_click(panel, false);
    lv_obj_set_size(panel, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(panel, 0, 0);
    lv_obj_set_user_data(panel, this);
    lv_obj_set_design_cb(panel, DashPanelDesign);
    lv_obj_set_hidden(panel, true);
  }
  for (auto& text : dashText_) {
    text = nullptr;
  }

  auto makeLabel = [this](lv_style_t* style, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h) -> lv_obj_t* {
    lv_obj_t* lbl = LvObjOrNull(lv_label_create(container_, nullptr));
    if (lbl) {
      lv_obj_add_style(lbl, LV_LABEL_PART_MAIN, style);
      if (w != 0) {
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CROP);
        lv_label_set_align(lbl, LV_LABEL_ALIGN_CENTER);
        lv_obj_set_size(lbl, w, h);
      }
      lv_obj_set_pos(lbl, x, y);
      lv_label_set_text_static(lbl, "");
      lv_obj_set_hidden(lbl, true);
    }
    return lbl;
  };
  // The status strip: ten pills 23 px apart, the check (recoloured) in slot 0.
  for (uint8_t i = 0; i < kDashPills; i++) {
    dash_[DashPill + i] = makeLabel(&haStyleDashPill_[HaBridge::DashV8::BandDoors], static_cast<lv_coord_t>(6 + 23 * i), 155, 20, 16);
  }
  dash_[DashCheck] = makeLabel(&haStyleStatusBold_, 6, 150, 0, 0);
  if (dash_[DashCheck]) lv_label_set_recolor(dash_[DashCheck], true);
  // The freezer bar, over the panel's rule.
  dash_[DashFreezer] = makeLabel(&haStyleDashBar_, 0, 175, 240, 20);
}

// ============================================================================
// HideAllGroups — hide every pre-created widget. Called before showing a new group.
// No allocations, no deletions — just lv_obj_set_hidden(true).
// ============================================================================
void HomeAssistant::HideAllGroups() {
  if (timeLabel_) lv_obj_set_hidden(timeLabel_, true);
  if (statusLabel_) lv_obj_set_hidden(statusLabel_, true);
  if (screenIndicatorLabel_) lv_obj_set_hidden(screenIndicatorLabel_, true);
  if (defaultHelpLabel_) lv_obj_set_hidden(defaultHelpLabel_, true);

  for (uint8_t i = 0; i < kMaxTiles; i++) {
    if (tileObjs_[i]) lv_obj_set_hidden(tileObjs_[i], true);
  }

  for (lv_obj_t* obj : dash_) {
    if (obj) lv_obj_set_hidden(obj, true);
  }

  // Reset tile tap bindings (will be re-bound by ConfigureTileGroup)
  for (uint8_t i = 0; i < tileTapCount_; ++i) {
    tileTapPool_[i].haService = nullptr;
  }
  tileTapCount_ = 0;

  // Clear click handlers from tiles
  for (uint8_t i = 0; i < kMaxTiles; i++) {
    if (tileObjs_[i]) {
      lv_obj_set_user_data(tileObjs_[i], nullptr);
      lv_obj_set_event_cb(tileObjs_[i], nullptr);
    }
  }

  activeGroup_ = ScreenGroup::None;
}

// ============================================================================
// Present — the whole render of one materialization, inside the heap-phase hold.
//
// The model returns Ready, NoLayout and NoMemory with the phase held (Activation), so
// nothing in the widget build or the draw can overlap a transfer. This builds the shared
// widgets the first time a draw is possible (container_ is null until then), evaluates
// safe-face mode (REQ-064) so the first render already respects low memory, draws, and
// only then ends the hold. Deferred holds nothing and builds nothing: every widget
// pointer stays null and the per-tick code is null-guarded until Refresh() succeeds.
// ============================================================================
void HomeAssistant::Present(Outcome outcome) {
  if (outcome == Outcome::Ready || outcome == Outcome::NoLayout || outcome == Outcome::NoMemory) {
    if (container_ == nullptr) {
      CreateSharedWidgets();
      CheckSafeFaceCondition();
    }
  }
  ShowScreen(outcome);
  model_.EndMaterialization();
  // The first store read of an unconfirmed boot has been drawn (a record, or none): the boot
  // confirmation may proceed. NoMemory does not release: no read happened, the model retries
  // it, and the hold stays until a read is drawn. Deferred never reads either.
  if (bootReadHeld_ && (outcome == Outcome::Ready || outcome == Outcome::NoLayout)) {
    bootReadHeld_ = false;
#if HA_BOOT_TRACE
    // Diagnostic: the first read is drawn (the confirmation follows on SystemTask).
    HaBridge::BootConfirm::MarkStep(6);
#endif
    HaBridge::BootConfirm::ReleaseFirstRead();
  }
}

// ============================================================================
// ShowScreen — draw the result of a materialization (HaBridge::ActiveScreenModel).
// Hides the current group and shows the new one; ZERO heap allocations once the
// group exists — only repositions, resizes, and updates text of existing widgets.
//
// Deferred is the unstable branch: a transfer holds the heap phase, a write is
// pending, or the generation moved. The last drawn screen stays exactly as it is
// and no widget group is created here, so the default group is never created
// while a layout is committed. The model keeps the request and Refresh() retries
// it: a deferred update is never dropped.
//
// The default group is drawn only for NoLayout (the store holds no committed
// layout), NoMemory or safe-face mode (error states), or when the layout asks for
// it (screenCount 0, or a dashboard screen with no dashboard data).
// ============================================================================
void HomeAssistant::ShowScreen(Outcome outcome) {
  switch (outcome) {
    case Outcome::Deferred:
      // Nothing was materialized. If nothing has been drawn yet, show the shared
      // time and status widgets only (no group is created).
      if (activeGroup_ == ScreenGroup::None) ShowPlaceholder();
      return;
    case Outcome::Unchanged:
    case Outcome::Inactive:
      return;
    default:
      break;
  }

  HideAllGroups();

  // Prepare time and status text buffers FIRST (before showing any labels)
  UpdateTimeAndStatus();

#if HA_BUILD_VARIANT_DEFAULT_ONLY
  // REQ-006 variant: only render the default group. Used to isolate whether
  // garble originates in tile/dashboard creation paths vs the default path.
  CreateDefaultGroupOnce();
  ConfigureDefaultGroup();
  return;
#endif

  // REQ-064 — safe-face mode: render only time + status when LVGL heap is
  // too fragmented (free_biggest < 1 KB) or an lv_*_create() has already
  // failed. The simplified default group needs far fewer LVGL objects than
  // a full tile or dashboard layout. Re-attempt next Refresh() automatically.
  // REQ-067 — banner text is "layout too heavy" so the user knows the layout
  // pushed the watch into safe mode (actionable: open preflight on Android).
  // A view that could not be allocated (NoMemory) draws the same safe face; the
  // model retries it each tick, and nothing latches.
  if (safeFaceMode_ || outcome == Outcome::NoMemory) {
    CreateDefaultGroupOnce();
    ConfigureDefaultGroup();
    if (defaultHelpLabel_) {
      lv_label_set_text_static(defaultHelpLabel_, "layout too heavy");
    }
    return;
  }

  const HaBridge::ActiveScreenView* view = model_.view();
  switch (HaBridge::ChooseGroup(view)) {
    case HaBridge::ScreenGroup::Default:
      // NoLayout, version 0, or screenCount 0.
      CreateDefaultGroupOnce();
      ConfigureDefaultGroup();
      return;
    case HaBridge::ScreenGroup::NoData:
      // Dashboard screen type but no dashboard data configured — show "No data"
      CreateDefaultGroupOnce();
      ConfigureDefaultGroup();
      if (defaultHelpLabel_) {
        lv_label_set_text_static(defaultHelpLabel_, kNoDataHelpText_);
      }
      break;
    case HaBridge::ScreenGroup::Dashboard:
      CreateDashboardGroupOnce();
      ConfigureDashboardGroup(*view);
      break;
    case HaBridge::ScreenGroup::Tiles:
      // Tile screens and REQ-069 quick_controls (wire type 3, read as a tile
      // screen) share the tile group; the per-tile confirm flag in the view is
      // what the phone set.
      CreateTileGroupOnce();
      ConfigureTileGroup(view->screen.tileView);
      break;
  }

  // Screen indicator if multiple screens; on the dashboard always ("1/1" too), at the top in its
  // quiet look (v8).
  const bool onDash = activeGroup_ == ScreenGroup::Dashboard;
  if ((view->common.screenCount > 1 || onDash) && screenIndicatorLabel_) {
    snprintf(screenIndBuf_, sizeof(screenIndBuf_), "%d/%d", model_.screen() + 1, view->common.screenCount);
    lv_label_set_text_static(screenIndicatorLabel_, screenIndBuf_);
    PlaceIndicator(onDash);
    lv_obj_set_hidden(screenIndicatorLabel_, false);
  }
}

// A first draw that had to wait: only the shared time and status widgets, at the
// default-group positions. No group is created and activeGroup_ stays None, so the
// per-tick updates that key off it stay off until a screen is drawn.
void HomeAssistant::ShowPlaceholder() {
  SetClockFont(&jetbrains_mono_42);
  UpdateTimeAndStatus();
  if (timeLabel_) {
    lv_obj_align(timeLabel_, container_, LV_ALIGN_IN_TOP_MID, 0, SCREEN_HEIGHT / 2 - 30);
    lv_obj_set_hidden(timeLabel_, false);
  }
  if (statusLabel_) {
    lv_obj_align(statusLabel_, container_, LV_ALIGN_IN_TOP_MID, 0, SCREEN_HEIGHT / 2 + 30);
    lv_obj_set_hidden(statusLabel_, false);
  }
}

// One read of the persisted layout record. The flash store (and the lfs_file_t inside
// it) lives on the stack for this call only; it is bound to the same FS.
int HomeAssistant::ReadAt(size_t offset, uint8_t* out, size_t len) {
  HaBridge::LayoutStoreFlash flash;
  flash.Bind(haService_.FileSystem());
  return flash.ReadAt(offset, out, len);
}

std::atomic<uint8_t>& HomeAssistant::PhaseCell() {
  return haService_.HeapPhaseByte();
}

// HaBridge::ViewSource over the service: the model reads the generation and the
// update signal through these and nothing else.
uint32_t HomeAssistant::LayoutGen() const {
  return haService_.GetLayoutGen();
}

bool HomeAssistant::StoreReadable() {
  return haService_.SystemAwake();
}

bool HomeAssistant::UpdatePending() const {
  return haService_.HasUpdate();
}

bool HomeAssistant::TakeUpdate() {
  return haService_.TakeUpdate();
}

// ============================================================================
// ConfigureDefaultGroup — show time, status, help text (no layout configured)
// ============================================================================
void HomeAssistant::ConfigureDefaultGroup() {
  // REQ-004 — font/color already provided by haStyleTime_ / haStyleStatusBold_
  // attached in CreateSharedWidgets. Only position + visibility here (and the big clock).
  SetClockFont(&jetbrains_mono_42);
  if (timeLabel_) {
    lv_obj_align(timeLabel_, container_, LV_ALIGN_IN_TOP_MID, 0, SCREEN_HEIGHT / 2 - 30);
    lv_obj_set_hidden(timeLabel_, false);
  }

  if (statusLabel_) {
    lv_obj_align(statusLabel_, container_, LV_ALIGN_IN_TOP_MID, 0, SCREEN_HEIGHT / 2 + 30);
    lv_obj_set_hidden(statusLabel_, false);
  }

  if (defaultHelpLabel_) {
    lv_label_set_text_static(defaultHelpLabel_, kDefaultHelpText_);
    lv_obj_align(defaultHelpLabel_, container_, LV_ALIGN_IN_TOP_MID, 0, SCREEN_HEIGHT - 50);
    lv_obj_set_hidden(defaultHelpLabel_, false);
  }

  activeGroup_ = ScreenGroup::Default;
}

// ============================================================================
// Dashboard v8 (2.4.98; docs/Plans/dashboard-v8-plan.md, docs/face_dashboard/v8/design-spec.md).
// Time first and quiet: one grey line above the clock, two event lines below it, then the house
// (a strip of pills, the freezer band, the thermostats, the people at home) and the watch's own.
//
//   y   6  date (grey)            1/4            weather icon, temperature, high/low
//   y  42  the clock, centred across 228 px                              jetbrains_mono_76
//   y 115  event 1: start (cyan)  title, cut to 146 px                   montserrat_14
//   y 133  event 2: start (grey)  title
//   y 155  the status strip: the doors check or a pill in each of ten slots
//   y 175  the freezer bar (an alarm only), over the rule at y 185       bold_20
//   y 196  thermostats                                home icon, count   montserrat_18
//   y 218  battery, steps, heart rate (icons grey, numbers white)        bold_20 / montserrat_18
//
// What to show is decided in HaBridge::DashV8 (host-tested); this maps it onto the objects. What
// changes comes from the live DASHBOARD_STATE (HaBridge::DashLive); before the first state the
// weather and the next event come from the layout record, greyed.
// ============================================================================
namespace {
  constexpr lv_coord_t kDashClockY = 42;  // the digits' tops at y 50, their baseline at y 106
  constexpr lv_coord_t kDashTitleW = 146;

  constexpr const char* kGlyphCheck = "\xEF\x80\x8C";  // FontAwesome check, in bold_20
  constexpr const char* kDegree = "\xC2\xB0";

  // The text a dashboard label or panel text gets next, built here before it is compared with
  // what is drawn. One buffer for all, since they are built one after another.
  char g_dashScratch[96];

  // The panel's texts (HomeAssistant::DashText order): box, font and alignment. The boxes are
  // those the labels had. None wraps (LV_TXT_FLAG_EXPAND): a LONG_CROP label sets the same flag
  // and crops what does not fit, as the clip to the box does here.
  struct DashBox {
    lv_coord_t x, y, w, h;
    const lv_font_t* font;
    lv_txt_flag_t flag;
  };
  const DashBox kDashBoxes[] = {
    {6, 6, 60, 17, &lv_font_montserrat_14, LV_TXT_FLAG_EXPAND},                           // date
    {150, 9, 30, 25, &fontawesome_weathericons, LV_TXT_FLAG_EXPAND | LV_TXT_FLAG_RIGHT},  // weather icon
    {184, 3, 50, 21, &lv_font_montserrat_18, LV_TXT_FLAG_EXPAND | LV_TXT_FLAG_RIGHT},     // temperature
    {184, 23, 50, 17, &lv_font_montserrat_14, LV_TXT_FLAG_EXPAND | LV_TXT_FLAG_RIGHT},    // high/low
    {6, 115, 78, 17, &lv_font_montserrat_14, LV_TXT_FLAG_EXPAND},                         // event 1 start
    {6, 133, 78, 17, &lv_font_montserrat_14, LV_TXT_FLAG_EXPAND},                         // event 2 start
    {88, 115, 146, 17, &lv_font_montserrat_14, LV_TXT_FLAG_EXPAND},                       // event 1 title
    {88, 133, 146, 17, &lv_font_montserrat_14, LV_TXT_FLAG_EXPAND},                       // event 2 title
    {6, 196, 90, 21, &lv_font_montserrat_18, LV_TXT_FLAG_EXPAND},                         // thermostats
    {180, 193, 26, 23, &jetbrains_mono_bold_20, LV_TXT_FLAG_EXPAND},                      // home icon
    {206, 196, 30, 21, &lv_font_montserrat_18, LV_TXT_FLAG_EXPAND},                       // at home
    {6, 215, 28, 23, &jetbrains_mono_bold_20, LV_TXT_FLAG_EXPAND},                        // battery icon
    {34, 218, 48, 21, &lv_font_montserrat_18, LV_TXT_FLAG_EXPAND},                        // battery
    {88, 215, 28, 23, &jetbrains_mono_bold_20, LV_TXT_FLAG_EXPAND},                       // steps icon
    {116, 218, 62, 21, &lv_font_montserrat_18, LV_TXT_FLAG_EXPAND},                       // steps, to the heart icon
    {180, 215, 26, 23, &jetbrains_mono_bold_20, LV_TXT_FLAG_EXPAND},                      // heart icon
    {206, 218, 30, 21, &lv_font_montserrat_18, LV_TXT_FLAG_EXPAND},                       // heart rate
  };
  // HomeAssistant::DashTone as colours.
  constexpr uint32_t kDashTones[] = {Pinetime::Applications::HaBridge::DashV8::kWhite,
                                     Pinetime::Applications::HaBridge::DashV8::kGrey,
                                     Pinetime::Applications::HaBridge::DashV8::kCyan,
                                     Pinetime::Applications::HaBridge::DashV8::kCharging};

  // Gives `obj` the shared style `style` in place of the one it carries. The style list's only
  // entry is replaced, so nothing is allocated (a remove and an add would reallocate the list
  // twice from the heap); a list holding anything else is rebuilt.
  void SwapStyle(lv_obj_t* obj, uint8_t part, lv_style_t* style) {
    lv_style_list_t* list = lv_obj_get_style_list(obj, part);
    if (list == nullptr) return;
    if (list->style_cnt == 1 && !list->has_local && !list->has_trans) {
      if (list->style_list[0] == style) return;
      list->style_list[0] = style;
      lv_obj_refresh_style(obj, part, LV_STYLE_PROP_ALL);
      return;
    }
    lv_obj_reset_style_list(obj, part);
    lv_obj_add_style(obj, part, style);
  }

  // The panel's rule (y 185), in its own frame so the label loop's stack stays small.
  __attribute__((noinline)) void DrawDashRule(lv_coord_t ox, lv_coord_t oy, const lv_area_t* clip) {
    lv_area_t area;
    lv_area_t part;
    lv_area_set(&area, ox + 6, oy + 185, ox + 233, oy + 185);
    if (!_lv_area_intersect(&part, clip, &area)) return;
    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.bg_color = lv_color_hex(Pinetime::Applications::HaBridge::DashV8::kRule);
    lv_draw_rect(&area, &part, &rect);
  }

  // HA's weather condition string (the layout's fallback) as a WeatherCode.
  uint8_t WeatherCodeOf(const char* cond) {
    using namespace Pinetime::Applications::HaBridge;
    if (cond == nullptr || cond[0] == '\0') return WxNone;
    static constexpr struct {
      const char* name;
      uint8_t code;
    } kCodes[] = {{"sunny", WxSunny}, {"clear", WxSunny}, {"clear-night", WxClearNight}, {"partlycloudy", WxPartlyCloudy},
                  {"cloudy", WxCloudy}, {"rainy", WxRainy}, {"pouring", WxPouring}, {"snowy", WxSnowy},
                  {"snowy-rainy", WxSnowyRainy}, {"hail", WxHail}, {"lightning", WxLightning},
                  {"lightning-rainy", WxLightningRainy}, {"windy", WxWindy}, {"windy-variant", WxWindyVariant},
                  {"fog", WxFog}, {"exceptional", WxExceptional}};
    for (const auto& c : kCodes) {
      if (strcmp(cond, c.name) == 0) return c.code;
    }
    return WxNone;
  }

  // A WeatherCode as a glyph of fontawesome_weathericons, or nullptr. The font has no moon:
  // a clear night shows the sun, as InfiniTime's own weather screen does.
  const char* WeatherIconOf(uint8_t code) {
    using namespace Pinetime::Applications::HaBridge;
    switch (code) {
      case WxSunny:
      case WxClearNight:
        return Symbols::sun;
      case WxPartlyCloudy:
        return Symbols::cloudSun;
      case WxCloudy:
      case WxWindy:
      case WxWindyVariant:
        return Symbols::cloud;
      case WxRainy:
        return Symbols::cloudSunRain;
      case WxPouring:
        return Symbols::cloudShowersHeavy;
      case WxSnowy:
      case WxSnowyRainy:
        return Symbols::snowflake;
      case WxHail:
        return Symbols::cloudMeatball;
      case WxLightning:
      case WxLightningRainy:
        return Symbols::bolt;
      case WxFog:
        return Symbols::smog;
      case WxExceptional:
        return Symbols::ban;
      default:
        return nullptr;
    }
  }

  // The width `len` bytes of text take in montserrat_14, kerning included: what LVGL measures
  // when it lays out an event title.
  int32_t Montserrat14Width(const char* text, size_t len, void* /*ctx*/) {
    return _lv_txt_get_width(text, static_cast<uint32_t>(len), &lv_font_montserrat_14, 0, LV_TXT_FLAG_NONE);
  }
}

void HomeAssistant::SetClockFont(const lv_font_t* font) {
  if (timeLabel_ == nullptr || clockFont_ == font) return;
  clockFont_ = font;
  lv_obj_set_style_local_text_font(timeLabel_, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, font);
}

void HomeAssistant::PlaceIndicator(bool dashboard) {
  if (screenIndicatorLabel_ == nullptr) return;
  if (indicatorOnDash_ != dashboard) {
    // haStyleDashInd_ (montserrat_14, faint) in place of haStyleScreenInd_ (its grey, the
    // theme's bold_20), and back: swapped, not allocated.
    indicatorOnDash_ = dashboard;
    SwapStyle(screenIndicatorLabel_, LV_LABEL_PART_MAIN, dashboard ? &haStyleDashInd_ : &haStyleScreenInd_);
  }
  if (dashboard) {
    lv_obj_align(screenIndicatorLabel_, container_, LV_ALIGN_IN_TOP_MID, 0, 6);
  } else {
    lv_obj_align(screenIndicatorLabel_, container_, LV_ALIGN_IN_BOTTOM_MID, 0, -2);
  }
}

bool HomeAssistant::RefreshDashLiveCopy() {
  const uint32_t gen = haService_.GetDashLiveGeneration();
  if (gen == 0) return false;
  if (gen == dashLiveCopyGen_) return true;
  uint32_t copied = 0;
  if (!haService_.ReadDashLive(dashLiveCopy_, &copied)) {
    dashLiveCopyGen_ = 0;
    return false;
  }
  dashLiveCopyGen_ = copied;
  return true;
}

void HomeAssistant::SetDashText(lv_obj_t* label, char* buf, size_t size, const char* text) {
  if (label == nullptr) return;
  const bool show = text[0] != '\0';
  if (show && strncmp(buf, text, size) != 0) {
    strncpy(buf, text, size - 1);
    buf[size - 1] = '\0';
    lv_label_set_text_static(label, buf);
  }
  if (lv_obj_get_hidden(label) == show) lv_obj_set_hidden(label, !show);
}

void HomeAssistant::SetPanelText(uint8_t item, char* buf, size_t size, const char* text, uint8_t tone) {
  const char* next = nullptr;
  bool changed = false;
  if (text != nullptr && text[0] != '\0') {
    next = text;
    if (buf != nullptr) {
      if (strncmp(buf, text, size) != 0) {
        strncpy(buf, text, size - 1);
        buf[size - 1] = '\0';
        changed = true;
      }
      next = buf;
    }
  }
  if (!changed && next == dashText_[item] && tone == dashTone_[item]) return;
  dashText_[item] = next;
  dashTone_[item] = tone;
  lv_obj_t* panel = dash_[DashPanel];
  if (panel == nullptr) return;
  // Redraw that text's box only (nothing when the panel is hidden).
  const DashBox& b = kDashBoxes[item];
  lv_area_t area;
  lv_area_set(&area, panel->coords.x1 + b.x, panel->coords.y1 + b.y, panel->coords.x1 + b.x + b.w - 1,
              panel->coords.y1 + b.y + b.h - 1);
  lv_obj_invalidate_area(panel, &area);
}

lv_design_res_t HomeAssistant::DashPanelDesign(lv_obj_t* obj, const lv_area_t* clip, lv_design_mode_t mode) {
  // Transparent: it never covers what is under it, and draws only its texts.
  if (mode == LV_DESIGN_COVER_CHK) return LV_DESIGN_RES_NOT_COVER;
  if (mode == LV_DESIGN_DRAW_MAIN) {
    auto* self = static_cast<HomeAssistant*>(lv_obj_get_user_data(obj));
    if (self != nullptr) self->DrawDashPanel(clip);
  }
  return LV_DESIGN_RES_OK;
}

// What lv_label_design draws for a cropping label, for each text whose box crosses the area
// being refreshed: lv_draw_label in the box, clipped to it.
void HomeAssistant::DrawDashPanel(const lv_area_t* clip) {
  static_assert(sizeof(kDashBoxes) / sizeof(kDashBoxes[0]) == kDashTexts, "a box per panel text");
  lv_obj_t* panel = dash_[DashPanel];
  if (panel == nullptr) return;
  const lv_coord_t ox = panel->coords.x1;
  const lv_coord_t oy = panel->coords.y1;
  DrawDashRule(ox, oy, clip);
  lv_draw_label_dsc_t dsc;
  lv_draw_label_dsc_init(&dsc);
  for (uint8_t i = 0; i < kDashTexts; i++) {
    if (dashText_[i] == nullptr) continue;
    const DashBox& b = kDashBoxes[i];
    lv_area_t area;
    lv_area_t part;
    lv_area_set(&area, ox + b.x, oy + b.y, ox + b.x + b.w - 1, oy + b.y + b.h - 1);
    if (!_lv_area_intersect(&part, clip, &area)) continue;
    dsc.font = b.font;
    dsc.flag = b.flag;
    dsc.color = lv_color_hex(kDashTones[dashTone_[i]]);
    lv_draw_label(&area, &part, &dsc, dashText_[i], nullptr);
  }
}

// The date and the two event lines. Live events: event 1's start cyan, event 2's grey, the titles
// white. Before the first state: the layout record's next event, while still ahead (its copy there
// is as old as the last layout push), all grey. No event: the lines stay empty.
void HomeAssistant::RenderDashTop(const HaBridge::ActiveScreenView& view, const HaBridge::DashLive* live,
                                  uint32_t nowEpoch) {
  namespace V8 = HaBridge::DashV8;
  dashMeetingMinute_ = nowEpoch / 60u;
  char* const text = g_dashScratch;
  snprintf(text, sizeof(g_dashScratch), "%s %u", dateTimeController_.DayOfWeekShortToString(),
           static_cast<unsigned>(dateTimeController_.Day()));
  SetPanelText(TxtDate, dashDateBuf_, sizeof(dashDateBuf_), text, ToneGrey);

  uint32_t start[HaBridge::DashLive::kEvents] = {};
  const char* title[HaBridge::DashLive::kEvents] = {};
  uint8_t count = 0;
  if (live != nullptr) {
    uint8_t idx[HaBridge::DashLive::kEvents] = {};
    count = V8::UpcomingEvents(*live, nowEpoch, idx);
    for (uint8_t k = 0; k < count; k++) {
      start[k] = live->eventStart[idx[k]];
      title[k] = live->eventTitle[idx[k]].data();
    }
  } else {
    const auto& common = view.common;
    const auto& dash = view.screen.dashView;
    for (uint8_t i = 0; i < common.eventCount && i < dash.events.size() && i < common.eventStartEpoch.size(); i++) {
      if (common.eventStartEpoch[i] > nowEpoch) {
        start[0] = common.eventStartEpoch[i];
        title[0] = dash.events[i].title.data();
        count = 1;
        break;
      }
    }
  }

  // Local time minus UTC, for the weekday and time of an event on another day.
  const int32_t utcOffset = static_cast<int32_t>(dateTimeController_.UtcOffset()) * 15 * 60;
  for (uint8_t k = 0; k < 2; k++) {
    text[0] = '\0';
    if (k < count) V8::FormatEventStart(text, sizeof(dashEvStartBuf_[k]), start[k], nowEpoch, utcOffset);
    SetPanelText(TxtEvStart + k, dashEvStartBuf_[k], sizeof(dashEvStartBuf_[k]), text,
                 (live != nullptr && k == 0) ? ToneCyan : ToneGrey);

    text[0] = '\0';
    if (k < count) {
      const char* t = (title[k] != nullptr && title[k][0] != '\0') ? title[k] : "event";
      V8::FitTitle(t, text, sizeof(dashEvTitleBuf_[k]), kDashTitleW, Montserrat14Width, nullptr);
    }
    SetPanelText(TxtEvTitle + k, dashEvTitleBuf_[k], sizeof(dashEvTitleBuf_[k]), text,
                 live != nullptr ? ToneWhite : ToneGrey);
  }
}

// The weather, the status strip, the freezer bar, the thermostats and the people at home.
void HomeAssistant::RenderDashLive(const HaBridge::ActiveScreenView& view, const HaBridge::DashLive* live,
                                   uint32_t nowEpoch) {
  namespace V8 = HaBridge::DashV8;
  const auto& dash = view.screen.dashView;
  const bool stale = live != nullptr && V8::IsStale(*live, nowEpoch);
  char* const text = g_dashScratch;

  // ── The weather: live, else the layout's condition and high/low, greyed. Stale leaves it be. ──
  {
    uint8_t code = 0;
    bool haveTemp = false;
    bool haveHiLo = false;
    int temp = 0;
    int hi = 0;
    int lo = 0;
    if (live != nullptr && live->hasWeather) {
      code = live->weatherCode;
      haveTemp = true;
      temp = live->weatherTemp;
      haveHiLo = live->hasHiLo;
      hi = live->weatherHi;
      lo = live->weatherLo;
    } else if (live == nullptr) {
      code = WeatherCodeOf(dash.weatherCondition.data());
      haveHiLo = code != 0 && !(dash.weatherHi == 0 && dash.weatherLo == 0);
      hi = dash.weatherHi;
      lo = dash.weatherLo;
    }
    const char* icon = WeatherIconOf(code);

    SetPanelText(TxtWxIcon, nullptr, 0, icon, live != nullptr ? ToneWhite : ToneGrey);

    text[0] = '\0';
    if (haveTemp && icon != nullptr) snprintf(text, sizeof(g_dashScratch), "%d%s", temp, kDegree);
    SetPanelText(TxtWxTemp, dashWxTempBuf_, sizeof(dashWxTempBuf_), text, ToneWhite);

    text[0] = '\0';
    if (haveHiLo && icon != nullptr) snprintf(text, sizeof(g_dashScratch), "%d/%d", hi, lo);
    SetPanelText(TxtWxHiLo, dashWxHiLoBuf_, sizeof(dashWxHiLoBuf_), text, ToneGrey);
  }

  // ── The status strip. From a live state computed for this layout (same fingerprint) only:
  // never from the record's flags, which are as old as the last layout push. ──
  {
    const bool matches = live != nullptr && live->fingerprint == HaBridge::StatusFingerprint(dash);
    // fpMis in SENSOR_REPORT: a state computed for another layout lights nothing here. `live` is
    // always dashLiveCopy_, so dashLiveCopyGen_ is its generation (the count is once per state).
    if (live != nullptr && !matches) haService_.CountFingerprintMiss(dashLiveCopyGen_);
    V8::Strip strip;
    if (matches) strip = V8::PackStatus(dash, *live);
    const uint8_t first = strip.check ? 1 : 0;
    for (uint8_t s = 0; s < kDashPills; s++) {
      text[0] = '\0';
      if (s >= first && s - first < strip.count) {
        const V8::Pill& pill = strip.pills[s - first];
        text[0] = pill.symbol;
        text[1] = '\0';
        // The fill: its band's shared style, or the grey one (stale), swapped in place.
        if (dash_[DashPill + s] != nullptr) {
          SwapStyle(dash_[DashPill + s], LV_LABEL_PART_MAIN, &haStyleDashPill_[stale ? V8::kBands : pill.band]);
        }
      }
      SetDashText(dash_[DashPill + s], dashPillBuf_[s], sizeof(dashPillBuf_[s]), text);
    }
    text[0] = '\0';
    if (strip.check) {
      snprintf(text, sizeof(g_dashScratch), "#%06lX %s#", static_cast<unsigned long>(V8::CheckColour(stale)), kGlyphCheck);
    }
    SetDashText(dash_[DashCheck], dashCheckBuf_, sizeof(dashCheckBuf_), text);
  }

  // ── The freezer bar, the thermostats (grey when stale), the count at home. Live only. ──
  {
    text[0] = '\0';
    if (live != nullptr) V8::FreezerText(*live, text, sizeof(dashFreezerBuf_));
    SetDashText(dash_[DashFreezer], dashFreezerBuf_, sizeof(dashFreezerBuf_), text);

    text[0] = '\0';
    if (live != nullptr) V8::ThermostatText(*live, text, sizeof(dashThermoBuf_));
    SetPanelText(TxtThermo, dashThermoBuf_, sizeof(dashThermoBuf_), text,
                 V8::ThermostatColour(stale) == V8::kGrey ? ToneGrey : ToneWhite);

    // The home icon always shows; the count only when a beacon has a reading.
    uint8_t home = 0;
    text[0] = '\0';
    if (live != nullptr && V8::PeopleHome(*live, home)) snprintf(text, sizeof(g_dashScratch), "%u", static_cast<unsigned>(home));
    SetPanelText(TxtHome, dashHomeBuf_, sizeof(dashHomeBuf_), text, ToneWhite);
  }
}

// The watch's own: battery (its icon by level, grey; the plug in green while power is present),
// steps in full, heart rate ("--" without a reading). Set only when the text changed.
void HomeAssistant::RenderDashboardBottomRow() {
  namespace V8 = HaBridge::DashV8;
  char* const text = g_dashScratch;
  const uint8_t batt = batteryController_.PercentRemaining();
  // The plug (U+F1E6) is in jetbrains_mono_bold_20, the battery icon's font (fonts.json, range 0xf1e6).
  // Power present, not "charging": a full battery on the cable still reads as plugged in.
  if (batteryController_.IsPowerPresent()) {
    SetPanelText(TxtBattIcon, nullptr, 0, Symbols::plug, ToneGreen);
  } else {
    SetPanelText(TxtBattIcon, nullptr, 0, V8::BatteryIcon(batt), ToneGrey);
  }
  snprintf(text, sizeof(g_dashScratch), "%u%%", static_cast<unsigned>(batt));
  SetPanelText(TxtBatt, dashBattBuf_, sizeof(dashBattBuf_), text, ToneWhite);

  snprintf(text, sizeof(g_dashScratch), "%lu", static_cast<unsigned long>(motionController_.NbSteps()));
  SetPanelText(TxtSteps, dashStepsBuf_, sizeof(dashStepsBuf_), text, ToneWhite);

  const uint8_t hr = heartRateController_.HeartRate();
  if (hr > 0) {
    snprintf(text, sizeof(g_dashScratch), "%u", static_cast<unsigned>(hr));
  } else {
    snprintf(text, sizeof(g_dashScratch), "--");
  }
  SetPanelText(TxtHeart, dashHeartBuf_, sizeof(dashHeartBuf_), text, ToneWhite);
}

void HomeAssistant::ConfigureDashboardGroup(const HaBridge::ActiveScreenView& view) {
  HA_LV_LOG("ConfigureDashboardGroup");  // REQ-005
  // The view is this screen's private copy: no generation check is needed to read it.
  const uint32_t nowEpoch = static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          dateTimeController_.UTCDateTime().time_since_epoch()).count());

  SetClockFont(&jetbrains_mono_76);
  if (timeLabel_) {
    // Aligned, not just placed: UpdateTimeAndStatus realigns the clock every second, and a
    // realign returns a label to its last alignment.
    lv_obj_align(timeLabel_, container_, LV_ALIGN_IN_TOP_MID, 0, kDashClockY);
    lv_obj_set_hidden(timeLabel_, false);
  }
  if (statusLabel_) lv_obj_set_hidden(statusLabel_, true);
  // The events are on the screen itself: the REQ-070 header shows on the other screens. The
  // REQ-065 banner would cover the watch's own row: here staleness greys the house instead.
  if (nextEventLabel_) lv_obj_set_hidden(nextEventLabel_, true);
  if (staleBanner_) lv_obj_set_hidden(staleBanner_, true);

  // Every text is set again from scratch (the buffers may hold another view's).
  for (auto& t : dashText_) t = nullptr;
  dashDateBuf_[0] = '\0';
  dashWxTempBuf_[0] = '\0';
  dashWxHiLoBuf_[0] = '\0';
  for (uint8_t k = 0; k < 2; k++) {
    dashEvStartBuf_[k][0] = '\0';
    dashEvTitleBuf_[k][0] = '\0';
  }
  for (auto& b : dashPillBuf_) b[0] = '\0';
  dashCheckBuf_[0] = '\0';
  dashFreezerBuf_[0] = '\0';
  dashThermoBuf_[0] = '\0';
  dashHomeBuf_[0] = '\0';
  dashBattBuf_[0] = '\0';
  dashStepsBuf_[0] = '\0';
  dashHeartBuf_[0] = '\0';

  // What never changes: three icons (the panel draws the rule).
  SetPanelText(TxtHomeIcon, nullptr, 0, Symbols::home, ToneGrey);
  SetPanelText(TxtStepsIcon, nullptr, 0, Symbols::shoe, ToneGrey);
  SetPanelText(TxtHeartIcon, nullptr, 0, Symbols::heartBeat, ToneGrey);
  if (dash_[DashPanel]) lv_obj_set_hidden(dash_[DashPanel], false);

  const bool haveLive = RefreshDashLiveCopy();
  dashLiveGenShown_ = haveLive ? dashLiveCopyGen_ : 0;
  RenderDashTop(view, haveLive ? &dashLiveCopy_ : nullptr, nowEpoch);
  RenderDashLive(view, haveLive ? &dashLiveCopy_ : nullptr, nowEpoch);
  RenderDashboardBottomRow();

  activeGroup_ = ScreenGroup::Dashboard;
}

// ============================================================================
// Tile screens (2.4.94): the 2 x 2 grid drawn as the Home Control app draws its devices.
// Dark framed tiles, the name (or icon) on top and the state below it; a tile that is on
// takes the accent: frame, state and a tint of it for the ground. No clock or status row
// (the dashboard has them), so the tiles fill the screen above the "n/N" indicator.
// Each tile is still the one label it was (no new objects): the frame is its border,
// the two lines one recoloured text, set apart by the line spacing.
// ============================================================================
namespace {
  constexpr lv_coord_t kTileLineSpace = 26;      // a one-line name: name at the top, state at the bottom
  constexpr lv_coord_t kTileLineSpaceTight = 2;  // a name that wraps: a block (the state lands at the same y)
  constexpr uint8_t kTileTopLines = 3;           // with the state, 4 lines fill the tile
  // As HaHomeControl: the ground, the frame and the dim state of a tile that is off.
  constexpr uint32_t kTileGround = 0x1A1A2E;
  constexpr uint32_t kTileFrame = 0x333355;
  constexpr uint32_t kTileStateOff = 0x8A8A8A;
  constexpr uint32_t kTileAccentDefault = 0xFFD700;  // the layout's default active colour
  constexpr uint8_t kTileGroundMix = 56;             // a lit tile's ground: 22 % accent on black
  constexpr uint32_t kTileResultOk = 0x34C759;
  constexpr uint32_t kTileResultFail = 0xFF453A;
  // FontAwesome check (U+F00C), in the tile font; Symbols::check is the double check.
  constexpr const char* kTileCheck = "\xEF\x80\x8C";
  constexpr const char* kTileConfirmPrompt = "confirm?";
  // tileFlags_ bits.
  constexpr uint8_t kTileHasEntity = 0x01;
  constexpr uint8_t kTileOwnColor = 0x02;  // a fixed colour, not coloured by state
  constexpr uint8_t kTileLit = 0x04;       // coloured by state, and on

  // A tile's icon as a symbol of jetbrains_mono_bold_20, or nullptr for the name. Only
  // symbols the font has: "light", "garage" and "fire" mapped to glyphs it lacks
  // (flashlight, a Material cog, bolt) and drew a blank line.
  const char* TileSymbol(const Pinetime::Controllers::HomeAssistantService::WatchTile& tile) {
    if (tile.displayMode != 1) return nullptr;
    const char* icon = tile.icon.data();
    if (strcmp(icon, "home") == 0) return Symbols::home;
    if (strcmp(icon, "music") == 0) return Symbols::music;
    if (strcmp(icon, "bell") == 0) return Symbols::bell;
    if (strcmp(icon, "heart") == 0) return Symbols::heartBeat;
    if (strcmp(icon, "bluetooth") == 0) return Symbols::bluetooth;
    if (strcmp(icon, "door") == 0) return Symbols::shieldAlt;
    if (strcmp(icon, "battery") == 0) return Symbols::batteryHalf;
    if (strcmp(icon, "steps") == 0) return Symbols::shoe;
    if (strcmp(icon, "tv") == 0) return Symbols::play;
    if (strcmp(icon, "fan") == 0) return Symbols::sun;
    if (strcmp(icon, "power") == 0) return Symbols::plug;
    if (strcmp(icon, "truck") == 0 || strcmp(icon, "car") == 0) return Symbols::map;
    if (strcmp(icon, "person_male") == 0 || strcmp(icon, "person_female") == 0) return Symbols::eye;
    return nullptr;
  }

  // "#RRGGBB" as 0xRRGGBB, or `fallback` for anything else (an unset colour is empty).
  uint32_t HexColor(const char* text, uint32_t fallback) {
    if (text[0] != '#') return fallback;
    uint32_t value = 0;
    for (int i = 1; i <= 6; i++) {
      const char c = text[i];
      uint32_t digit;
      if (c >= '0' && c <= '9') {
        digit = static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = static_cast<uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        digit = static_cast<uint32_t>(c - 'A' + 10);
      } else {
        return fallback;
      }
      value = (value << 4) | digit;
    }
    return value;
  }

  // Copies `text` for a recoloured label, outside any colour span: '#' doubled (a literal
  // there), cut at a character boundary to fit.
  void CopyEscaped(char* out, size_t size, const char* text) {
    size_t used = 0;
    size_t k = 0;
    while (text[k] != '\0') {
      size_t len = 1;
      while ((static_cast<uint8_t>(text[k + len]) & 0xC0) == 0x80) len++;  // the rest of a UTF-8 character
      const bool hash = text[k] == '#';
      if (used + (hash ? 2u : len) >= size) break;
      if (hash) out[used++] = '#';
      std::memcpy(out + used, text + k, len);
      used += len;
      k += len;
    }
    out[used] = '\0';
  }

  // Breaks the (escaped) top text with a '\n' where LVGL wraps it at `width`, keeping at
  // most kTileTopLines lines. The tile label crops, and a cropped label in LVGL 7 does not
  // wrap: a name wider than the tile was cut at both ends.
  void WrapTop(char* text, size_t size, const lv_font_t* font, lv_coord_t width) {
    size_t start = 0;
    uint8_t lines = 1;
    while (text[start] != '\0') {
      size_t end = start + _lv_txt_get_next_line(text + start, font, 0, width, LV_TXT_FLAG_RECOLOR);
      if (text[end] == '\0') break;
      if (lines == kTileTopLines) {
        text[end] = '\0';
        break;
      }
      if (text[end - 1] == ' ') {
        text[end - 1] = '\n';  // a word break: the space ending the line
      } else {
        // A word longer than the line, broken inside: insert the newline.
        const size_t length = std::strlen(text);
        if (length + 2 > size) {
          text[end] = '\0';
          break;
        }
        std::memmove(text + end + 1, text + end, length - end + 1);
        text[end++] = '\n';
      }
      start = end;
      lines++;
    }
  }

  // Appends `text` as the text of the open colour span and closes it, on one line: '_' as
  // a space, a '#' left out (inside a span it would end it), and when it is wider than
  // `width` cut to what fits with a '.' ("unavailable" -> "unavail.").
  void AppendSpanText(char* buf, size_t size, size_t used, const char* text, const lv_font_t* font, lv_coord_t width) {
    if (used + 3 > size) used = size - 3;  // room for a character, the closing '#' and the end
    const size_t start = used;
    for (size_t k = 0; text[k] != '\0' && used + 3 <= size; k++) {
      if (text[k] != '#') buf[used++] = text[k] == '_' ? ' ' : text[k];
    }
    const auto widthOf = [&](size_t end) {
      return _lv_txt_get_width(buf + start, static_cast<uint32_t>(end - start), font, 0, LV_TXT_FLAG_NONE);
    };
    if (widthOf(used) > width) {
      const lv_coord_t dotW = static_cast<lv_coord_t>(lv_font_get_glyph_width(font, '.', '\0'));
      while (used > start) {
        do {
          used--;  // the last character, all of its UTF-8 bytes
        } while (used > start && (static_cast<uint8_t>(buf[used]) & 0xC0) == 0x80);
        if (widthOf(used) + dotW <= width) break;
      }
      while (used > start && buf[used - 1] == ' ') used--;
      buf[used++] = '.';
    }
    buf[used++] = '#';
    buf[used] = '\0';
  }
}

// ============================================================================
// ConfigureTileGroup — reposition/restyle pre-created tiles for the given screen.
// No widget creation — only set_pos/set_size/set_hidden/set_text.
// ============================================================================
void HomeAssistant::ConfigureTileGroup(const HaBridge::ActiveScreenView::TileView& tileView) {
  // The view is this screen's private copy: no generation check is needed to read it.
  const uint8_t tileCount = tileView.tileCount < kMaxTiles ? tileView.tileCount : kMaxTiles;
  // A tile coloured by its state lights up in the layout's active colour; the inactive
  // colour (default purple) no longer fills the tile: off is the plain dark tile.
  const uint32_t stateAccent = HexColor(tileView.defaultActiveColor.data(), kTileAccentDefault);

  for (uint8_t i = 0; i < tileCount; i++) {
    lv_obj_t* tile = tileObjs_[i];
    if (tile == nullptr) continue;
    const auto& tileData = tileView.tiles[i];

    lv_obj_set_size(tile, kTileW, kTileH);
    lv_obj_set_pos(tile, static_cast<lv_coord_t>(kTileMargin + (i % 2) * (kTileW + kTileGap)),
                   static_cast<lv_coord_t>(kTileMargin + (i / 2) * (kTileH + kTileGap)));
    // REQ-004 — the chrome (cover, radius, frame width, insets, bold_20, white text) is the
    // shared haStyleTileChrome_; RenderTile sets the colours, the line spacing and pad_top.
    lv_obj_add_style(tile, LV_LABEL_PART_MAIN, &haStyleTileChrome_);
    lv_label_set_recolor(tile, true);
    lv_obj_set_click(tile, true);

    const char* symbol = TileSymbol(tileData);
    CopyEscaped(tileLabelTopBuf_[i], sizeof(tileLabelTopBuf_[i]), symbol != nullptr ? symbol : tileData.label.data());
    WrapTop(tileLabelTopBuf_[i], sizeof(tileLabelTopBuf_[i]), &jetbrains_mono_bold_20, kTileTextW);
    const bool hasEntity = tileData.entityId.data()[0] != '\0';
    // A tile with a colour of its own (not coloured by state) keeps it as its frame.
    tileAccent_[i] = tileData.colorEnabled ? stateAccent : HexColor(tileData.color.data(), kTileAccentDefault);
    tileFlags_[i] = static_cast<uint8_t>((hasEntity ? kTileHasEntity : 0u) | (tileData.colorEnabled ? 0u : kTileOwnColor));
    tileStateBuf_[i][0] = '\0';
    if (hasEntity) {
      ReadTileState(i, tileData.entityId.data());
    }
    RenderTile(i);

    // Click handler
    if (hasEntity && tileTapCount_ < kMaxTileTaps) {
      auto& tapData = tileTapPool_[tileTapCount_];
      tapData.haService = &haService_;
      std::strncpy(tapData.entityId, tileData.entityId.data(), sizeof(tapData.entityId) - 1);
      tapData.entityId[sizeof(tapData.entityId) - 1] = '\0';
      tapData.confirm = tileData.confirm;
      tapData.tileIndex = i;
      tapData.self = this;
      tileTapCount_++;
      lv_obj_set_user_data(tile, &tapData);
      lv_obj_set_event_cb(tile, OnTileTapped);
    }

    lv_obj_set_hidden(tile, false);
  }

  activeGroup_ = ScreenGroup::Tiles;
}

bool HomeAssistant::ReadTileState(uint8_t i, const char* entityId) {
  char state[sizeof(tileStateBuf_[0])];
  haService_.GetEntityState(entityId, state, sizeof(state));
  if (std::strcmp(state, tileStateBuf_[i]) == 0) return false;
  std::memcpy(tileStateBuf_[i], state, sizeof(state));
  const bool lit = (tileFlags_[i] & kTileOwnColor) == 0 && IsEntityActive(state);
  tileFlags_[i] = static_cast<uint8_t>(lit ? (tileFlags_[i] | kTileLit) : (tileFlags_[i] & ~kTileLit));
  return true;
}

void HomeAssistant::RenderTile(uint8_t i, const char* note, uint32_t noteColor) {
  lv_obj_t* tile = tileObjs_[i];
  if (tile == nullptr) return;
  const uint8_t flags = tileFlags_[i];
  const bool lit = (flags & kTileLit) != 0;
  const uint32_t accent = tileAccent_[i];
  lv_obj_set_style_local_bg_color(tile, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT,
                                  lit ? lv_color_mix(lv_color_hex(accent), LV_COLOR_BLACK, kTileGroundMix)
                                      : lv_color_hex(kTileGround));
  lv_obj_set_style_local_border_color(tile, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT,
                                      lv_color_hex((lit || (flags & kTileOwnColor) != 0) ? accent : kTileFrame));

  // The second line: the note, else the state (on: the accent; off or not known yet: dim).
  const char* line2 = note;
  uint32_t color = noteColor;
  if (line2 == nullptr && (flags & kTileHasEntity) != 0) {
    line2 = tileStateBuf_[i][0] != '\0' ? tileStateBuf_[i] : "---";
    color = lit ? accent : kTileStateOff;
  }

  const lv_font_t* font = &jetbrains_mono_bold_20;
  const lv_coord_t lineH = lv_font_get_line_height(font);
  // The top: the icon, or the name, broken into lines by WrapTop.
  lv_coord_t lines = 1;
  for (const char* c = tileLabelTopBuf_[i]; *c != '\0'; ++c) {
    if (*c == '\n') lines++;
  }
  const lv_coord_t space = lines > 1 ? kTileLineSpaceTight : kTileLineSpace;
  if (line2 == nullptr) {
    lv_label_set_text_static(tile, tileLabelTopBuf_[i]);
  } else {
    const int used = snprintf(tileCombinedBuf_[i], sizeof(tileCombinedBuf_[i]), "%s\n#%06lX ", tileLabelTopBuf_[i],
                              static_cast<unsigned long>(color & 0xFFFFFFu));
    AppendSpanText(tileCombinedBuf_[i], sizeof(tileCombinedBuf_[i]), static_cast<size_t>(used), line2, font, kTileTextW);
    lv_label_set_text_static(tile, tileCombinedBuf_[i]);
    lines++;
  }
  lv_obj_set_style_local_text_line_space(tile, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, space);
  const lv_coord_t textH = static_cast<lv_coord_t>(lines * lineH + (lines - 1) * space);
  lv_obj_set_style_local_pad_top(tile, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT,
                                 static_cast<lv_coord_t>(textH < kTileH ? (kTileH - textH) / 2 : 0));
}

void HomeAssistant::UpdateTimeAndStatus() {
  // Every common-section decision below is a function of the view and the clock
  // (HaBridge, ActiveScreenView.h): the clock format, the next-event header and its
  // vibration request, and the stale banner.
  const HaBridge::ActiveScreenView* view = model_.view();
  const bool is24h = HaBridge::Is24Hour(view);
  const uint16_t year = dateTimeController_.Year();
  const auto nowSec = static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          dateTimeController_.UTCDateTime().time_since_epoch()).count());

  uint8_t hour = dateTimeController_.Hours();
  uint8_t minute = dateTimeController_.Minutes();

  if (!is24h) {
    hour = hour % 12;
    if (hour == 0) hour = 12;
  }

  if (timeLabel_) {
    snprintf(timeBuf_, sizeof(timeBuf_), "%02d:%02d", hour, minute);
    lv_label_set_text_static(timeLabel_, timeBuf_);
    lv_obj_realign(timeLabel_);
  }

  uint8_t batteryPct = batteryController_.PercentRemaining();

  if (statusLabel_) {
    uint8_t hr = heartRateController_.HeartRate();
    uint32_t steps = motionController_.NbSteps();
    // While power is present the plug (U+F1E6, in this label's font) leads the percentage, in green.
    char plug[16] = "";
    if (batteryController_.IsPowerPresent()) {
      snprintf(plug, sizeof(plug), "#%06lX %s#", static_cast<unsigned long>(HaBridge::DashV8::kCharging), Symbols::plug);
    }
    if (hr > 0) {
      snprintf(statusBuf_, sizeof(statusBuf_), "%s%d%% HR %d STP %lu",
               plug, batteryPct, hr, (unsigned long)steps);
    } else {
      snprintf(statusBuf_, sizeof(statusBuf_), "%s%d%% HR -- STP %lu",
               plug, batteryPct, (unsigned long)steps);
    }
    lv_label_set_text_static(statusLabel_, statusBuf_);
    lv_obj_realign(statusLabel_);
  }

  // REQ-070 — "next event in Xm" header.
  //
  // DecideNextEvent checks the two calendar events for one whose start epoch is
  // within the next 30 minutes and shows "next in Xm". When the epoch has just
  // been crossed it requests a one-shot vibration, drained by
  // BridgeController::OnTick through haService_.RequestCalendarVibration().
  // Re-evaluation of the vibration is throttled to once per minute bucket.
  if (nextEventLabel_) {
    // The live state's events when there is one (2.4.96): the layout's are as old as the
    // last layout push, and its vibration would ring for events long moved or gone.
    const auto decision =
      RefreshDashLiveCopy()
        ? HaBridge::DecideNextEventAt(dashLiveCopy_.eventStart.data(), dashLiveCopy_.eventCount,
                                      HaBridge::LayoutPresent(view), year, nowSec, nextEventThrottle_)
        : HaBridge::DecideNextEvent(view, year, nowSec, nextEventThrottle_);
    if (decision.requestVibration) {
      haService_.RequestCalendarVibration();
    }
    // On the dashboard zone A carries the countdown.
    const bool showNextEvent = decision.show && activeGroup_ != ScreenGroup::Dashboard;
    if (showNextEvent) {
      snprintf(nextEventBuf_, sizeof(nextEventBuf_), "next in %lum", (unsigned long)decision.minutes);
    }
    if (showNextEvent) {
      lv_label_set_text_static(nextEventLabel_, nextEventBuf_);
      lv_obj_set_hidden(nextEventLabel_, false);
    } else {
      lv_obj_set_hidden(nextEventLabel_, true);
    }
  }

  // REQ-065 — stale-data banner: evaluate staleness and update the overlay
  // strip at the bottom of every HA screen.
  //
  // "HA offline" when the phone's flag is set (haService_.IsDataStale(): ha_offline or
  // data_stale); "No phone Xm" when nothing has come from the phone for more than
  // kBannerNoContactSec (900 s), X counted from the last message: HaBridge::IsBannerStale.
  if (staleBanner_) {
    const bool haStale = haService_.IsDataStale();
    const uint32_t sinceContact = haService_.SecondsSinceLastRx();
    // Not on the dashboard (v8): its bottom row is the watch's own numbers, and a live state
    // that stops coming greys the house there instead.
    const bool onDash = activeGroup_ == ScreenGroup::Dashboard;
    const bool stale = !onDash && HaBridge::IsBannerStale(haStale, sinceContact);

    if (stale) {
      if (haStale) {
        snprintf(staleBannerBuf_, sizeof(staleBannerBuf_), "HA offline");
      } else {
        snprintf(staleBannerBuf_, sizeof(staleBannerBuf_), "No phone %lum",
                 static_cast<unsigned long>(sinceContact / 60u));
      }
      lv_label_set_text_static(staleBanner_, staleBannerBuf_);
      lv_obj_set_hidden(staleBanner_, false);
      // The banner is full-width and pinned to LV_ALIGN_IN_BOTTOM_MID, which is
      // the same slot the "n/N" screen indicator uses (offset -2). They were
      // painting over each other — on hardware the red banner sat across the
      // indicator. There is no room to stack them: the bottom date/steps/HR row
      // ends at y=212 and the indicator occupies ~224..238, leaving 12 px for a
      // 14 px banner. So they take turns. A stale-data warning outranks knowing
      // which screen you are on, and the indicator returns the moment BT
      // recovers.
      if (screenIndicatorLabel_) {
        lv_obj_set_hidden(screenIndicatorLabel_, true);
      }
    } else {
      lv_obj_set_hidden(staleBanner_, true);
      // Restore the indicator only when there is more than one screen to
      // indicate — same condition ShowScreen uses to reveal it in the first
      // place, so a single-screen layout does not gain a stray "1/1" (the dashboard shows it).
      if (screenIndicatorLabel_ != nullptr && HaBridge::LayoutPresent(view) &&
          (view->common.screenCount > 1 || onDash)) {
        // Left in the dashboard's look (a safe face drawn after it): back to the bottom.
        if (indicatorOnDash_ != onDash) PlaceIndicator(onDash);
        lv_obj_set_hidden(screenIndicatorLabel_, false);
      }
    }
  }
}

void HomeAssistant::Refresh() {
  // REQ-060 — display subsystem heartbeat.  Called every 1000 ms by taskRefresh_
  // (see BridgeController::kTickPeriodMs, which every OnTick interval derives
  // from) while this watch face is on-screen and the display is awake; a hung
  // display task stops calling this, letting the watchdog gate in OnTick detect
  // the missing Display bit.
  // UpdateDisplayStackHwm() wires the REQ-058 reserved displayStackHwm field.
#if HA_WATCHDOG_SUBSYSTEM_AWARE
  Pinetime::Applications::HaBridge::WatchdogSubsystemKick(
    Pinetime::Applications::HaBridge::WatchdogSubsystem::Display);
#endif
#if HA_CRASH_CONTEXT_EXTENDED
  Pinetime::Applications::HaBridge::UpdateDisplayStackHwm();
#endif

#if HA_BOOT_TRACE
  if (bootReadHeld_ && lastRefreshTick_ == 0) {
    HaBridge::BootConfirm::MarkStep(3);
  }
#endif
  haService_.ProcessTick();
#if HA_BOOT_TRACE
  // Diagnostic: the first tick of an unconfirmed boot (the read is still owed) has run.
  if (bootReadHeld_ && lastRefreshTick_ == 0) {
    HaBridge::BootConfirm::MarkStep(4);
  }
#endif

  // REQ-064 — re-evaluate safe-face conditions every tick so the face
  // recovers automatically when LVGL fragmentation improves.
  CheckSafeFaceCondition();

  // Wake detection: reset to screen 0 if display was sleeping >3s. The reset is a
  // materialization like any other: it goes through the model, and a deferral (a
  // transfer holds the heap phase) is retried by model_.Refresh() below.
  uint32_t now = lv_tick_get();
  const bool notOnFirst = model_.screen() != 0 || (model_.HasPending() && model_.PendingScreen() != 0);
  if (lastRefreshTick_ != 0 && (now - lastRefreshTick_) > 3000 && notOnFirst) {
    lastRefreshTick_ = now;
    Present(model_.SwitchTo(0));
    return;
  }
  lastRefreshTick_ = now;

  // Retry a deferred request, else serve a raised update signal (the redraw after a
  // commit). The model takes the heap phase, and consumes the signal only after a
  // stable read has been committed into the view; a deferral leaves it raised.
  {
#if HA_BOOT_TRACE
    if (bootReadHeld_) {
      HaBridge::BootConfirm::MarkStep(5);
    }
#endif
    const Outcome outcome = model_.Refresh();
    if (outcome != Outcome::Unchanged) {
      Present(outcome);
      if (outcome != Outcome::Deferred) return;
    }
  }

  // Update time display
  UpdateTimeAndStatus();

  // Tile confirm timeout check — restore original text after 2 seconds
  if (confirmPendingTileIndex_ >= 0) {
    if ((lv_tick_get() - confirmStartTick_) > 2000) {
      RestoreConfirmTileText(static_cast<uint8_t>(confirmPendingTileIndex_));
      confirmPendingTileIndex_ = -1;
    }
  }

  // REQ-068 — poll for ACTION_RESULT and update tile overlay (✓/✗/⌛).
  ProcessActionResult();

  // Lightweight dashboard dynamic data update (steps, HR, battery, date, notif count)
  UpdateDashboardDynamic();

  // Lightweight tile color update
  UpdateTileColors();
}

bool HomeAssistant::OnTouchEvent(TouchEvents event) {
  const HaBridge::ActiveScreenView* view = model_.view();
  if (!HaBridge::LayoutPresent(view) || view->common.screenCount <= 1) return false;

  // A deferred request is owed: the next swipe moves on from where the face is heading.
  const uint8_t current = model_.HasPending() ? model_.PendingScreen() : model_.screen();
  uint8_t next = current;
  switch (event) {
    case TouchEvents::SwipeLeft:
      if (current >= view->common.screenCount - 1) return false;
      next = static_cast<uint8_t>(current + 1);
      break;

    case TouchEvents::SwipeRight:
      if (current == 0) return false;
      next = static_cast<uint8_t>(current - 1);
      break;

    default:
      return false;
  }
  // A swipe is a materialization: through the model, which takes the heap phase.
  // Deferred keeps the last drawn screen and the model retries it from Refresh().
  Present(model_.SwitchTo(next));
  return true;
}

// SYNC: This active-state list must match Android WatchFaceOrchestrator.kt::isEntityActive().
// If you add/remove states here, update the Android side too.
bool HomeAssistant::IsEntityActive(const char* state) {
  if (state == nullptr || state[0] == '\0') return false;
  if (strcmp(state, "on") == 0) return true;
  if (strcmp(state, "open") == 0) return true;
  if (strcmp(state, "home") == 0) return true;
  if (strcmp(state, "playing") == 0) return true;
  if (strcmp(state, "heat") == 0) return true;
  if (strcmp(state, "cool") == 0) return true;
  if (strcmp(state, "heat_cool") == 0) return true;
  if (strcmp(state, "auto") == 0) return true;
  if (strcmp(state, "unlocked") == 0) return true;
  if (strcmp(state, "above_horizon") == 0) return true;
  if (strcmp(state, "fan_only") == 0) return true;
  if (strcmp(state, "dry") == 0) return true;
  return false;
}

// ============================================================================
// UpdateDashboardDynamic — per-tick update for dashboard labels.
// Updates date, battery, steps, HR, notification count — no allocations.
// ============================================================================
void HomeAssistant::UpdateDashboardDynamic() {
  if (activeGroup_ != ScreenGroup::Dashboard) return;
  RenderDashboardBottomRow();

  const HaBridge::ActiveScreenView* view = model_.view();
  if (view == nullptr) return;
  const uint32_t nowEpoch = static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          dateTimeController_.UTCDateTime().time_since_epoch()).count());
  // A new DASHBOARD_STATE (an even generation not drawn yet), or a new minute: the
  // countdown moves and the stale check may grey the status.
  const uint32_t gen = haService_.GetDashLiveGeneration();
  const bool newState = gen != 0 && (gen & 1u) == 0 && gen != dashLiveGenShown_;
  const bool newMinute = nowEpoch / 60u != dashMeetingMinute_;
  if (!newState && !newMinute) return;

  const bool haveLive = RefreshDashLiveCopy();
  // A copy that raced a write: what is drawn stays until the next tick reads again.
  if (!haveLive && dashLiveGenShown_ != 0) return;
  if (haveLive) dashLiveGenShown_ = dashLiveCopyGen_;
  RenderDashTop(*view, haveLive ? &dashLiveCopy_ : nullptr, nowEpoch);
  RenderDashLive(*view, haveLive ? &dashLiveCopy_ : nullptr, nowEpoch);
}

void HomeAssistant::UpdateTileColors() {
  if (activeGroup_ != ScreenGroup::Tiles) return;
  const HaBridge::ActiveScreenView* view = model_.view();
  if (!HaBridge::LayoutPresent(view) || view->common.screenCount == 0) return;
  // REQ-069 — quick_controls (wire type 3) reads as a tile screen in the view.
  if (view->common.screenType != 1) return;

  const auto& tileView = view->screen.tileView;
  const uint8_t tileCount = tileView.tileCount < kMaxTiles ? tileView.tileCount : kMaxTiles;

  for (uint8_t i = 0; i < tileCount; i++) {
    if (tileObjs_[i] == nullptr || (tileFlags_[i] & kTileHasEntity) == 0) continue;
    if (!ReadTileState(i, tileView.tiles[i].entityId.data())) continue;
    // REQ-024 — a tile with a pending tap-to-confirm keeps its prompt, and one showing an
    // action result its glyph, until they expire (2 s): they are redrawn then, with the
    // state read here. Without this a STATE_UPDATE mid-confirm overwrote the prompt and
    // the user believed their tap was lost.
    if (confirmPendingTileIndex_ == static_cast<int8_t>(i) || (actionResultCode_ != 0 && actionResultTileIndex_ == i)) {
      continue;
    }
    RenderTile(i);
  }
}

// ============================================================================
// REQ-068 — ProcessActionResult
//
// Called every Refresh() tick (1 s).  Polls the cross-task signal in
// HomeAssistantService and shows the result on the invoking tile's state line:
//
//   Phase 1 — new result arrives (actionResultCode_ == 0, service code != 0):
//     Take ownership of the signal (ClearActionResult), record tile + start tick,
//     draw the result in place of the state (RenderTile's note).
//
//   Phase 2 — overlay is active (actionResultCode_ != 0):
//     After 2 seconds, call RestoreConfirmTileText to return the tile to its
//     name and state, and reset state.
//
// Cohabits with REQ-024 confirm marker: OnTileTapped already restores the tile
// text before firing InvokeAction for the confirm-second-tap path, so the
// overlay never races with an active confirm prompt.
// ============================================================================
void HomeAssistant::ProcessActionResult() {
  // Phase 2: overlay is already active — check 2 s expiry.
  if (actionResultCode_ != 0) {
    if ((lv_tick_get() - actionResultStartTick_) > 2000) {
      if (actionResultTileIndex_ < kMaxTiles) {
        RestoreConfirmTileText(actionResultTileIndex_);
      }
      actionResultCode_      = 0;
      actionResultTileIndex_ = 0xFF;
    }
    return;
  }

  // Phase 1: poll for a new result from the BLE task.
  uint8_t tileIdx = 0xFF;
  uint8_t code = haService_.GetActionResult(tileIdx);
  if (code == 0 || tileIdx >= kMaxTiles) return;

  // Take ownership — clear the service-side signal so it is not re-read.
  haService_.ClearActionResult();

  if (tileObjs_[tileIdx] == nullptr) return;

  // code 1 = pending (shown immediately on tap, before the reply arrives): an hourglass
  // code 2 = success: a check
  // code 3 = failure: "failed"
  // The glyphs are FontAwesome symbols of the tile font. Until 2.4.94 these were the
  // Unicode ⌛ ✓ ✗, which the font does not have: the result line was blank.
  if (code == 2) {
    RenderTile(tileIdx, kTileCheck, kTileResultOk);
  } else if (code == 3) {
    RenderTile(tileIdx, "failed", kTileResultFail);
  } else {
    RenderTile(tileIdx, Symbols::hourGlass, tileAccent_[tileIdx]);
  }

  // Record overlay state so Phase 2 can time it out.
  actionResultCode_      = code;
  actionResultTileIndex_ = tileIdx;
  actionResultStartTick_ = lv_tick_get();
}

// Return a tile from the confirm prompt or an action result to its name and state.
void HomeAssistant::RestoreConfirmTileText(uint8_t tileIndex) {
  if (tileIndex >= kMaxTiles) return;
  RenderTile(tileIndex);
}

void HomeAssistant::OnTileTapped(lv_obj_t* obj, lv_event_t event) {
  if (event != LV_EVENT_CLICKED) return;
  auto* data = static_cast<TileTapData*>(lv_obj_get_user_data(obj));
  if (data == nullptr || data->haService == nullptr || data->entityId[0] == '\0') return;

  auto* self = static_cast<HomeAssistant*>(data->self);

  if (!data->confirm || self == nullptr) {
    // No confirm required — fire immediately.
    // REQ-068: record tile index before invoke so BridgeController can route ACTION_RESULT.
    data->haService->SetPendingActionTileIndex(data->tileIndex);
    data->haService->SetPendingTileEntity(data->entityId);
    data->haService->InvokeAction(0xFFFD, "tap");
    return;
  }

  const uint8_t tileIdx = data->tileIndex;

  if (self->confirmPendingTileIndex_ == tileIdx) {
    // Second tap on the same tile within timeout — fire action.
    // REQ-068: record tile index before invoke.
    data->haService->SetPendingActionTileIndex(tileIdx);
    data->haService->SetPendingTileEntity(data->entityId);
    data->haService->InvokeAction(0xFFFD, "tap");
    self->RestoreConfirmTileText(tileIdx);
    self->confirmPendingTileIndex_ = -1;
  } else {
    // First tap, or tap on a different tile — cancel any existing pending confirm first
    if (self->confirmPendingTileIndex_ >= 0) {
      self->RestoreConfirmTileText(static_cast<uint8_t>(self->confirmPendingTileIndex_));
    }
    // Set this tile as pending: the prompt replaces its state, in its accent.
    self->RenderTile(tileIdx, kTileConfirmPrompt, self->tileAccent_[tileIdx]);
    self->confirmPendingTileIndex_ = static_cast<int8_t>(tileIdx);
    self->confirmStartTick_ = lv_tick_get();
  }
}
