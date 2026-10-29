#pragma once
#include <array>
#include <atomic>
#include "displayapp/apps/Apps.h"
#include "displayapp/screens/Screen.h"
#include "displayapp/Controllers.h"
#include "components/ble/HomeAssistantService.h"
#include "ha_bridge/ActiveScreenView.h"
#include "ha_bridge/DashboardV8.h"
#include "ha_bridge/LayoutStore.h"
#include "Symbols.h"
namespace Pinetime {
  namespace Controllers {
    class Battery;
    class HeartRateController;
    class MotionController;
  }

  namespace Applications {
    namespace Screens {

      class HomeAssistant : public Screen, private HaBridge::ViewSource {
      public:
        HomeAssistant(Controllers::DateTime& dateTimeController,
                      const Controllers::Battery& batteryController,
                      Controllers::HeartRateController& heartRateController,
                      Controllers::MotionController& motionController,
                      Controllers::HomeAssistantService& haService);
        ~HomeAssistant() override;
        bool OnTouchEvent(TouchEvents event) override;
        void Refresh() override;

        // Tile tap data — each visible tile stores its entity ID for tap-to-toggle
        struct TileTapData {
          Controllers::HomeAssistantService* haService;
          char entityId[64];
          bool confirm {false};
          uint8_t tileIndex {0};
          void* self {nullptr};  // HomeAssistant* back-pointer for confirm flow
        };

      private:
        // REQ-030 — wrap every lv_*_create() return through this helper. On
        // null (LVGL pool exhausted), bumps haService_.lvCreateFailures_ so
        // REQ-064's safe-face trigger has a counter to read. Caller still
        // null-checks the return; the wrapper just centralizes the tally.
        lv_obj_t* LvObjOrNull(lv_obj_t* obj);

        // REQ-064 — free-heap threshold for safe-face mode.
        // Trip condition: xPortGetFreeHeapSize() < kFreeHeapSafeThresholdBytes
        //   OR  haService_.GetLvCreateFailures() > 0.
        //
        // PRIOR BUG: this read lv_mem_monitor().free_biggest_size, which is
        // ALWAYS 0 under LV_MEM_CUSTOM=1 (LVGL allocates via pvPortMalloc, so its
        // internal monitor is never populated). free_biggest_size < threshold was
        // therefore always true → the guard tripped every tick → the HA dashboard
        // showed "layout too heavy" permanently, even after the firmware accepted
        // the (now app-trimmed, 1-screen/2-tile) layout. Switched to the real
        // FreeRTOS free-heap figure.
        //
        // Threshold sizing — now grounded in real telemetry (compact HELLO diag,
        // fw 2.4.30): idle free heap on the HA face is ~11.2 KB; after the dashboard
        // group is built (CreateDashboardGroupOnce — widgets are created ONCE and
        // persist) free settles at ~5.5 KB with a min-ever of ~5.2 KB and NO crash.
        // The dashboard therefore fits and runs stably; the previous 6 KB threshold
        // was set ABOVE that stable point, so the guard tripped the tick after the
        // dashboard rendered (5560 < 6144) — the user saw the dashboard for a beat,
        // then "layout too heavy". Because the widgets persist, free heap is stable
        // (no per-tick re-render, no oscillation), so a single low floor is correct.
        // 3 KB sits ~2 KB below the observed min — the dashboard stays shown, and we
        // only fall back to the safe face on genuine near-exhaustion. The build
        // itself happens at idle (~11 KB), far above any OOM risk.
        static constexpr uint32_t kFreeHeapSafeThresholdBytes = 3072;  // 3 KB

        // Check trip conditions and update safeFaceMode_. Increments
        // haService_.safeFaceTrips_ on each false→true transition.
        void CheckSafeFaceCondition();

        Controllers::DateTime& dateTimeController_;
        const Controllers::Battery& batteryController_;
        Controllers::HeartRateController& heartRateController_;
        Controllers::MotionController& motionController_;
        Controllers::HomeAssistantService& haService_;

        lv_obj_t* container_ = nullptr;
        lv_task_t* taskRefresh_ = nullptr;

        // Navigation state (screen index, deferred request) lives in model_, below.
        // Per-activation baseline of haService_'s lvCreateFailures_ (which stays
        // cumulative for telemetry): the safe-face trip compares against it.
        uint16_t lvCreateBaseline_ = 0;

        // Wake detection
        uint32_t lastRefreshTick_ = 0;

        // REQ-064 — safe-face mode. Set true when LVGL fragmentation is severe
        // (free_biggest < 1024 B) or any lv_*_create() has returned null. While
        // true, ShowScreen() renders the simplified default group instead of the
        // full tile/dashboard layout. Re-evaluated every Refresh() tick so the
        // face recovers automatically when memory improves.
        bool safeFaceMode_ = false;

        // Screen group tracking — which widget group is currently visible
        enum class ScreenGroup : uint8_t { None, Default, Tiles, Dashboard };
        ScreenGroup activeGroup_ = ScreenGroup::None;

        // --- Widget pointers (created lazily, never destroyed until ~HomeAssistant) ---

        // Shared across all screen types
        lv_obj_t* timeLabel_ = nullptr;
        lv_obj_t* statusLabel_ = nullptr;
        lv_obj_t* screenIndicatorLabel_ = nullptr;

        // REQ-065 — stale-data banner: shown on every HA screen when the phone reports
        // HA offline, or nothing has come from the phone for 15 min
        // (HaBridge::IsBannerStale). Positioned as a fixed bottom strip (y = 224,
        // height = 16), always drawn over the active group.
        lv_obj_t* staleBanner_ = nullptr;
        char staleBannerBuf_[20] = {};

        // REQ-070 — "next event in Xm" header label. Shown at the top of every
        // HA screen when a calendar event (eventStartEpoch > 0) is within 30
        // minutes of the current UTC time. Hidden otherwise. Vibration at epoch
        // crossing is triggered via haService_.RequestCalendarVibration(), which
        // BridgeController::OnTick drains so the display task never blocks.
        lv_obj_t* nextEventLabel_ = nullptr;
        char nextEventBuf_[28] = {};
        // The once-per-minute evaluation state and the "vibration already fired"
        // flag are one throttle, updated by HaBridge::DecideNextEvent. The check
        // recalculates once per minute instead of every 20 ms tick and does not
        // re-fire every tick after the epoch crosses zero.
        HaBridge::NextEventThrottle nextEventThrottle_;

        // Default group
        lv_obj_t* defaultHelpLabel_ = nullptr;

        // Tile group — each tile is a single lv_label with bg styling (no container + child labels)
        static constexpr uint8_t kMaxTiles = 4;  // 2x2 grid; must equal storage kMaxTilesPerScreen
        static_assert(kMaxTiles <= std::tuple_size<decltype(HaBridge::ActiveScreenView::TileView::tiles)>::value,
                      "render tile cap must not exceed storage backing");
        lv_obj_t* tileObjs_[kMaxTiles] = {};

        // Dashboard group (v8, 2.4.98; docs/Plans/dashboard-v8-plan.md, the element table in
        // docs/face_dashboard/v8/design-spec.md). Every object is created and placed once; a
        // refresh sets the texts and styles that changed and shows or hides objects. The clock
        // is the shared timeLabel_ and the screen indicator the shared screenIndicatorLabel_.
        // Every LVGL object and style comes out of the FreeRTOS heap (LV_MEM_CUSTOM), so the
        // plain texts are not labels: one object, the panel, draws them all from dashText_.
        // The pills, the check and the freezer bar are labels, each with one shared style.
        static constexpr uint8_t kDashPills = HaBridge::DashV8::kSlots;
        enum DashObj : uint8_t {
          DashPanel = 0,                   // the plain texts and the rule (DrawDashPanel)
          DashPill,                        // kDashPills, slot order
          DashCheck = DashPill + kDashPills,
          DashFreezer,                     // the bar and its text: one label
          kDashObjs
        };
        // The panel's texts, in drawing order; their boxes, fonts and alignment are kDashBoxes.
        enum DashText : uint8_t {
          TxtDate = 0,
          TxtWxIcon,
          TxtWxTemp,
          TxtWxHiLo,
          TxtEvStart,                      // two: event 1, event 2
          TxtEvTitle = TxtEvStart + 2,     // two
          TxtThermo = TxtEvTitle + 2,
          TxtHomeIcon,
          TxtHome,
          TxtBattIcon,
          TxtBatt,
          TxtStepsIcon,
          TxtSteps,
          TxtHeartIcon,
          TxtHeart,
          kDashTexts
        };
        enum DashTone : uint8_t { ToneWhite = 0, ToneGrey, ToneCyan, ToneGreen };
        lv_obj_t* dash_[kDashObjs] = {};
        const char* dashText_[kDashTexts] = {};      // what the panel draws (null: nothing)
        uint8_t dashTone_[kDashTexts] = {};          // its colour (DashTone)
        const lv_font_t* clockFont_ = nullptr;       // timeLabel_'s font (SetClockFont); null: the style's
        bool indicatorOnDash_ = false;               // screenIndicatorLabel_ wears haStyleDashInd_
        uint32_t dashMeetingMinute_ = 0;           // epoch minute the date and events were drawn for
        uint32_t dashLiveGenShown_ = 0;              // DASHBOARD_STATE generation drawn (0: none)
        // The live state's copy the face draws from: static (one face at a time), so ~150 B
        // stay off the display task's stack and the heap. dashLiveCopyGen_ is the generation
        // it holds (0: none, or a read that raced a write left it mixed).
        static HaBridge::DashLive dashLiveCopy_;
        uint32_t dashLiveCopyGen_ = 0;

        // --- Tile tap data pool (value type, no heap allocation) ---
        static constexpr uint8_t kMaxTileTaps = 4;
        std::array<TileTapData, kMaxTileTaps> tileTapPool_{};
        uint8_t tileTapCount_ = 0;

        // Tile confirm-flow state (-1 = no confirm pending)
        int8_t confirmPendingTileIndex_ {-1};
        uint32_t confirmStartTick_ {0};

        // REQ-068 — action result overlay state.
        // resultCode: 0=none, 1=pending(⌛), 2=ok(✓), 3=fail(✗).
        // The overlay cohabits with the confirm marker (REQ-024): if
        // the tile was in confirm-pending state the overlay supersedes it
        // once the ACTION_RESULT arrives (confirm was resolved by the second tap).
        uint8_t  actionResultTileIndex_ {0xFF};  // 0xFF = no overlay
        uint32_t actionResultStartTick_ {0};
        uint8_t  actionResultCode_      {0};

        // --- TEXT FORMATTING BUFFERS (persistent — used with lv_label_set_text_static to avoid LVGL heap churn) ---
        char timeBuf_[6] = {};
        char statusBuf_[48] = {};
        char screenIndBuf_[8] = {};
        char tileCombinedBuf_[kMaxTiles][64] = {};  // "TOP\n#RRGGBB state#"
        char tileStateBuf_[kMaxTiles][32] = {};
        char tileLabelTopBuf_[kMaxTiles][28] = {};   // the icon, or the name ('#' doubled, '\n' at its wraps)
        uint32_t tileAccent_[kMaxTiles] = {};        // 0xRRGGBB: frame and state of a lit tile
        uint8_t tileFlags_[kMaxTiles] = {};          // kTileHasEntity | kTileOwnColor | kTileLit
        static constexpr char kDefaultHelpText_[] = "configure face\nin bridge app";
        static constexpr char kNoDataHelpText_[] = "No data";
        // The dashboard's texts (the icons are constant strings, pointed at by dashText_).
        char dashDateBuf_[8] = {};            // "WED 31"
        char dashWxTempBuf_[8] = {};          // "-12°"
        char dashWxHiLoBuf_[12] = {};         // "-12/-15"
        char dashEvStartBuf_[2][12] = {};     // "Wed 10:30"
        char dashEvTitleBuf_[2][32] = {};     // cut to 146 px: "Parent teacher co..."
        char dashPillBuf_[kDashPills][2] = {};
        char dashCheckBuf_[16] = {};          // "#2F7D4F <check>#" (recoloured: green, or faint when stale)
        char dashFreezerBuf_[20] = {};        // "!-12°", "21 freezers offline"
        char dashThermoBuf_[16] = {};         // "-12° -12°"
        char dashHomeBuf_[4] = {};
        char dashBattBuf_[6] = {};            // "100%"
        char dashStepsBuf_[8] = {};           // "23456", in full
        char dashHeartBuf_[4] = {};

        // REQ-004 — shared style registry. Each style holds font + color +
        // chrome properties once and is lv_obj_add_style()-attached to every
        // widget that needs the combination. Replaces ~45 per-widget
        // lv_obj_set_style_local_* calls, dropping LVGL pool fragmentation.
        // Styles are init'd once in CreateSharedWidgets and reset in the
        // dtor (lv_style_reset returns the heap allocation).
        // C2 (memory hardening): shared styles are STATIC + init-once + NEVER reset.
        // Per-renavigation lv_style_init/lv_style_reset churn fragmented the ~24 KB
        // heap (the documented InfiniTime G7710 crash class) and was a primary cause
        // of the LVGL-OOM NULL-deref hardfault when the HA face was reconstructed on
        // navigate-back. Persisting them for the process lifetime is a few small,
        // stable allocations. LVGL is single-threaded (displayapp task only), so
        // static sharing across the (single-at-a-time) HA instance is safe.
        static lv_style_t haStyleTime_;            // jetbrains_mono_bold_20 + WHITE
        static lv_style_t haStyleStatusBold_;      // jetbrains_mono_bold_20 + 0x999999 (also the recoloured check)
        static lv_style_t haStyleScreenInd_;       // 0x606060 text only
        static lv_style_t haStyleTileChrome_;      // cover, radius 10, 2 px frame, 4 px insets, bold_20 WHITE
        static lv_style_t haStyleContainerChrome_; // black bg + cover + 0 border/radius/pad
        // The pills: montserrat_14 black on a cover fill, radius 8; one per band, then grey (stale).
        static lv_style_t haStyleDashPill_[HaBridge::DashV8::kBands + 1];
        static lv_style_t haStyleDashBar_;         // the freezer bar: #FF453A fill, bold_20 black
        static lv_style_t haStyleDashInd_;         // the indicator on the dashboard: montserrat_14 #48484A
        static bool haStylesInitialized_;

        // --- Lazy group creation flags (true = group widgets already allocated) ---
        bool defaultGroupCreated_ = false;
        bool tileGroupCreated_ = false;
        bool dashboardGroupCreated_ = false;
        // True while this face holds the boot confirmation until its first store read is drawn
        // (BootConfirm::HoldForFirstRead). Shares the padding of the three flags above.
        bool bootReadHeld_ = false;

        // --- The active screen's view (REQ009 Path B) ---
        // The face renders from a per-screen view materialized out of the flash
        // layout store, never from a resident copy of the whole layout. Every
        // materialization (activation, swipe, the commit redraw, the wake reset)
        // goes through model_, which holds the heap phase while it reads the store.
        using Outcome = HaBridge::ActiveScreenModel::Outcome;
        // HaBridge::ViewSource over the service (the model reads only these three).
        uint32_t LayoutGen() const override;
        bool UpdatePending() const override;
        bool TakeUpdate() override;
        // False while the system is not Running: the flash may be powered down, so the model
        // defers instead of reading.
        bool StoreReadable() override;
        std::atomic<uint8_t>& PhaseCell() override;
        // The display task's read-only view of the record. The flash store and its 84 B
        // lfs_file_t are constructed on the stack inside each read, so no file handle (and
        // no FS pointer) is resident for the face's lifetime: the FS comes from haService_.
        int ReadAt(size_t offset, uint8_t* out, size_t len) override;
        HaBridge::ActiveScreenModel model_;

        // --- METHODS ---
        void CreateSharedWidgets();
        void CreateDefaultGroupOnce();
        void CreateTileGroupOnce();
        void CreateDashboardGroupOnce();
        void HideAllGroups();
        // Build (the first time), draw and end the hold of a materialization: the model returns
        // Ready, NoLayout and NoMemory with the heap phase held, and it is released here, after
        // the widgets are built and drawn. A deferred first draw builds nothing.
        void Present(Outcome outcome);
        // Draw the result of a materialization. Deferred keeps the last drawn screen.
        void ShowScreen(Outcome outcome);
        // Time and status only (shared widgets), for a first draw that had to wait.
        void ShowPlaceholder();
        void ConfigureDefaultGroup();
        void ConfigureTileGroup(const HaBridge::ActiveScreenView::TileView& tileView);
        void ConfigureDashboardGroup(const HaBridge::ActiveScreenView& view);
        void UpdateDashboardDynamic();
        // The dashboard's parts: the date and the events (each minute), the live parts (weather,
        // pills, freezer bar, thermostats, presence; each new DASHBOARD_STATE, and each minute for
        // the stale check), the watch's own (battery, steps, heart rate; each tick, set when the
        // text changed). `live` is null before the first state.
        void RenderDashTop(const HaBridge::ActiveScreenView& view, const HaBridge::DashLive* live, uint32_t nowEpoch);
        void RenderDashLive(const HaBridge::ActiveScreenView& view, const HaBridge::DashLive* live, uint32_t nowEpoch);
        void RenderDashboardBottomRow();
        // Sets a dashboard label's text when it changed, and shows it only when non-empty.
        void SetDashText(lv_obj_t* label, char* buf, size_t size, const char* text);
        // A panel text: copied into `buf` (or, with `buf` null, the constant `text` itself) and
        // redrawn when it or its tone changed. Empty or null: not drawn.
        void SetPanelText(uint8_t item, char* buf, size_t size, const char* text, uint8_t tone);
        // The panel's drawing: the rule, then every text that crosses the area being drawn.
        static lv_design_res_t DashPanelDesign(lv_obj_t* obj, const lv_area_t* clip, lv_design_mode_t mode);
        void DrawDashPanel(const lv_area_t* clip);
        // dashLiveCopy_ brought up to the latest state, copied only when a newer one was
        // published; false when there is none, or the copy raced a write (tried again next time).
        bool RefreshDashLiveCopy();
        // The clock's font: jetbrains_mono_76 (dashboard), jetbrains_mono_42 (default face) or bold_20.
        void SetClockFont(const lv_font_t* font);
        // The screen indicator's look and place: montserrat_14 #48484A at the top on the dashboard,
        // else as it always was (bold_20 grey at the bottom).
        void PlaceIndicator(bool dashboard);
        // Tile screens: reads each tile's state and redraws the tiles whose state changed.
        void UpdateTileColors();
        // Reads tile i's state into tileStateBuf_ (and its lit flag); true when it changed.
        bool ReadTileState(uint8_t i, const char* entityId);
        // Draws tile i (ground, frame, the top line and the state line) from tileFlags_,
        // tileAccent_ and the buffers. A `note` replaces the state line, in `noteColor`
        // (the confirm prompt, an action result).
        void RenderTile(uint8_t i, const char* note = nullptr, uint32_t noteColor = 0);
        void UpdateTimeAndStatus();
        static bool IsEntityActive(const char* state);

        void RestoreConfirmTileText(uint8_t tileIndex);
        // REQ-068 — poll action result signal from HomeAssistantService and
        // show/clear the ✓/✗/⌛ overlay on the correct tile.
        void ProcessActionResult();
        static void OnTileTapped(lv_obj_t* obj, lv_event_t event);
      };

    } // namespace Screens

    template <>
    struct WatchFaceTraits<WatchFace::HomeAssistant> {
      static constexpr WatchFace watchFace = WatchFace::HomeAssistant;
      static constexpr const char* name = "Home Assistant";

      static Screens::Screen* Create(AppControllers& controllers) {
        return new Screens::HomeAssistant(controllers.dateTimeController,
                                          controllers.batteryController,
                                          controllers.heartRateController,
                                          controllers.motionController,
                                          *controllers.homeAssistantController);
      };

      // HomeAssistant watchface re-enabled (2.4.26). The 42px-time garble on
      // re-render under the tight HA heap is fixed by using jetbrains_mono_bold_20
      // for the time (see HomeAssistant.cpp CreateSharedWidgets) — small fonts
      // render cleanly under the same pressure. Digital remains userWatchFaces[0]
      // (resource-free unconditional boot fallback); a persisted-HA selection that
      // ever fails construction still falls through to Digital via the boot find_if
      // isAvailable filter. Larger clock + full dashboard headroom await the
      // REQ-009 view-model RAM re-budget (the resident layout .bss is freed).
      static bool IsAvailable(Pinetime::Controllers::FS& /*filesystem*/) {
        return true;
      };
    };

  } // namespace Applications
} // namespace Pinetime
