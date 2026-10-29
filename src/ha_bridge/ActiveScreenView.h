#pragma once

// ActiveScreenView: the data the display task needs to render the ONE active
// HA screen, replacing reads of the resident WatchFaceLayout.
//
// Shape (REQ009-FR-B2):
//   common      - fields read whichever screen type is active. Outside any
//                 union, so the stale banner, the 12/24 h clock, the
//                 next-event label (and its calendar vibration) and the swipe
//                 bound keep working on a tile screen.
//   screen      - dashboard members and tile members share storage: only one
//                 screen type is active at a time. The active member is chosen
//                 by common.screenType (0 = dashboard, anything else = tiles,
//                 matching HomeAssistant::ShowScreen).
//
// Every member is a field the render path (the HomeAssistant screen's dashboard
// and tile drawing) reads; the view holds those and nothing else. A field the
// render path never reads is not here (for example the presence icons, the theme
// colours, the status-bar flags, the group type names, the notification list).
// Add a member only together with the code that draws it.
//
// No pointers: the view is a value copied out of the decode target, so its
// lifetime is independent of it.
//
// Host-linkable: no LVGL, and no NimBLE or FreeRTOS use in this header. The
// only dependency is the layout structs and caps in HomeAssistantService.h,
// whose NimBLE and FreeRTOS includes host builds satisfy through
// tools/fuzz/hoststubs (the same arrangement LayoutDecoder.h relies on).
// ActiveScreenView.cpp adds the store seam (LayoutStore.h) and the heap calls
// (pvPortMalloc / vPortFree) and nothing else.
//
// Storage: heap-allocated when the HA face activates and freed when it
// deactivates (the alternative, a static in .bss, would hold the bytes for the
// whole time the face is not on screen). The struct itself does not allocate;
// ActiveScreenModel below owns the allocation.
//
// The union members are not activated by construction here. Whoever fills the
// view selects the member by (re)constructing it or by zeroing the object
// before writing it.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <tuple>

#include "../components/ble/HomeAssistantService.h"

namespace Pinetime {
  namespace Applications {
    namespace HaBridge {

      class LayoutStore;

      struct ActiveScreenView {
        using Service = Pinetime::Controllers::HomeAssistantService;

        // Dashboard event slots, status groups and sensor groups are dimensioned
        // from the decode target so a cap change there cannot silently
        // truncate here.
        static constexpr std::size_t kEventSlots = std::tuple_size<decltype(Service::Dashboard::events)>::value;
        static constexpr std::size_t kStatusGroups = Service::kMaxStatusGroups;
        static constexpr std::size_t kGlyphsPerGroup = Service::kMaxGlyphsPerGroup;
        static constexpr std::size_t kSensorGroups = std::tuple_size<decltype(Service::Dashboard::sensorGroups)>::value;
        static constexpr std::size_t kSensorsPerGroup = std::tuple_size<decltype(Service::SensorGroup::values)>::value;

        // Read on every screen type (inventory rows 1-3, 5-8).
        struct Common {
          // Inventory row 6: stale banner, every screen.
          uint32_t lastEnrichedAt {0};
          // Inventory row 8: the two next-event epochs UpdateTimeAndStatus reads on
          // every screen (only what that function reads, not the events array).
          std::array<uint32_t, kEventSlots> eventStartEpoch {};
          // Inventory row 1: a layout is present and non-zero.
          uint8_t version {0};
          // Inventory row 2: indicator and swipe bound.
          uint8_t screenCount {0};
          // Inventory row 3: type of the active screen; selects the union member.
          uint8_t screenType {0};
          // Inventory row 5: first character of timeFormat ('H' = 24 h).
          char timeFormatLead {'\0'};
          // Inventory row 7: number of valid eventStartEpoch entries (0..kEventSlots).
          uint8_t eventCount {0};
        };

        // Dashboard screen only (inventory rows 4, 9-14). Compact copies of the
        // decode-target structs without the fields the render path never reads.
        struct DashEvent {
          std::array<char, 8> time {};
          std::array<char, 32> title {};
        };

        struct DashStatusGroup {
          std::array<Service::StatusGlyph, kGlyphsPerGroup> glyphs {};
          uint8_t glyphCount {0};
        };

        struct DashSensorGroup {
          std::array<int16_t, kSensorsPerGroup> values {};
          uint8_t valueCount {0};
        };

        struct DashView {
          bool hasDashboard {false};
          // The event count the dashboard render reads is Common::eventCount.
          std::array<DashEvent, kEventSlots> events {};
          Service::Dashboard::ConfigurableValue configurableValue {};
          std::array<Service::Dashboard::InlineMetric, 2> inlineMetrics {};
          std::array<char, 16> weatherCondition {};
          int8_t weatherHi {0};
          int8_t weatherLo {0};
          uint8_t statusGroupCount {0};
          std::array<DashStatusGroup, kStatusGroups> statusGroups {};
          uint8_t sensorGroupCount {0};
          std::array<DashSensorGroup, kSensorGroups> sensorGroups {};
        };

        // Tile screens only (inventory rows 15-21). WatchTile is read in full.
        struct TileView {
          // Global dynamic-colour defaults (inventory row 15): read only on tile paths.
          std::array<char, 8> defaultActiveColor {};
          std::array<char, 8> defaultInactiveColor {};
          uint8_t tileCount {0};
          std::array<Service::WatchTile, Service::kMaxTilesPerScreen> tiles {};
        };

        union Screen {
          DashView dashView;
          TileView tileView;
          Screen() : dashView() {}
        };

        Common common {};
        Screen screen {};
      };

      static_assert(sizeof(ActiveScreenView) <= 1536, "REQ009-NFR-2: ActiveScreenView must not exceed 1536 B");

      // ---------------------------------------------------------------------
      // C8c: the common-section decisions (REQ009-FR-C8c, FR-B2)
      //
      // Pure functions of the view and the wall clock. A null view is "no
      // layout" (the face is inactive, storage could not be allocated, or the
      // store holds no record): every decision then takes its no-layout answer.
      // The outcomes are those of HomeAssistant::UpdateTimeAndStatus.
      // ---------------------------------------------------------------------

      constexpr uint32_t kBannerNoContactSec = 900u;   // 15 min without a message from the phone
      constexpr uint32_t kNextEventWindowSec = 1800u;  // 30 min
      constexpr uint16_t kMinValidYear = 2024u;        // an unsynced RTC reads earlier

      /** A layout is present (inventory row 1). */
      inline bool LayoutPresent(const ActiveScreenView* v) {
        return v != nullptr && v->common.version != 0;
      }

      /** 12/24 h clock: 24 h unless a layout is present and its time format does not start with 'H'. */
      bool Is24Hour(const ActiveScreenView* v);

      /**
       * The stale banner. `haStale` is the service's ha_offline/data_stale flag: the phone says
       * Home Assistant is unreachable. Otherwise the banner shows when nothing has come from the
       * phone for more than kBannerNoContactSec; it pings every 30 s while connected.
       *
       * Until 2.4.93 the age was the dashboard's lastEnrichedAt against the clock, which only a
       * full dashboard push renews (a layout-record write). Once 2.4.92 gave the clock its zone,
       * the banner showed 15 min after every push while the link and the data were fine.
       */
      bool IsBannerStale(bool haStale, uint32_t secondsSinceContact);

      /** The next-event evaluation's once-per-minute state. */
      struct NextEventThrottle {
        // 31 bits hold the UTC minute until the year 4000; the flag shares the word (4 B, not 8).
        int32_t lastMinute : 31;
        uint32_t vibrated : 1;
        NextEventThrottle() : lastMinute(-1), vibrated(0) {
        }
      };

      struct NextEventDecision {
        bool show {false};             // show the "next in Xm" header
        uint32_t minutes {0};          // the X (0 once the start epoch has just passed)
        bool requestVibration {false}; // fire the calendar vibration
      };

      /**
       * Next-event selection over eventStartEpoch[0..min(eventCount, 2)) and the calendar
       * vibration request. The first event that starts within the next 30 minutes shows its
       * minutes (at least 1); one that started within the last 60 s shows 0 and requests one
       * vibration per minute bucket. `throttle` is updated only while a layout is present and
       * the year is valid, as the display code did.
       */
      NextEventDecision DecideNextEvent(const ActiveScreenView* v, uint16_t year, int64_t nowSec,
                                        NextEventThrottle& throttle);

      /**
       * DecideNextEvent over the given start epochs, `count` of them (at most 2 are read): the
       * live dashboard state's events when the face has them (2.4.96), whose layout copies are
       * as old as the last layout push. `layoutPresent` stands for LayoutPresent(v).
       */
      NextEventDecision DecideNextEventAt(const uint32_t* startEpochs, uint8_t count, bool layoutPresent,
                                          uint16_t year, int64_t nowSec, NextEventThrottle& throttle);

      /**
       * The status fingerprint (DashboardState.h) of the dashboard a view shows: FNV-1a over
       * each status group's glyph count and symbols. A DASHBOARD_STATE's bits are drawn only
       * when its fingerprint matches.
       */
      uint32_t StatusFingerprint(const ActiveScreenView::DashView& dash);

      /** Which widget group a view draws (the choice HomeAssistant::ShowScreen makes). */
      enum class ScreenGroup : uint8_t {
        Default,  // no layout, version 0, or screenCount 0
        NoData,   // a dashboard screen with no dashboard data
        Dashboard,
        Tiles
      };

      ScreenGroup ChooseGroup(const ActiveScreenView* v);

      // ---------------------------------------------------------------------
      // C8c: materialization and the view lifecycle
      // ---------------------------------------------------------------------

      /**
       * What the model reads of its owner: the layout generation, the update signal, the heap
       * phase cell and the store's bytes. One interface, so the model holds one reference.
       */
      class ViewSource {
       public:
        /** The heap phase cell the writer and the model share. */
        virtual std::atomic<uint8_t>& PhaseCell() = 0;
        /** LayoutStore::ReadAt over the persisted record (read-only). */
        virtual int ReadAt(size_t offset, uint8_t* out, size_t len) = 0;
        /** layoutGen_: odd while a write is publishing, even when stable. */
        virtual uint32_t LayoutGen() const = 0;
        /** The update signal is raised (HomeAssistantService::HasUpdate). */
        virtual bool UpdatePending() const = 0;
        /** Consume the signal (HomeAssistantService::TakeUpdate). True when it was raised. */
        virtual bool TakeUpdate() = 0;
        /**
         * False while the store cannot be read (the system is not running; in always-on display
         * the flash is powered down): a materialization is then deferred, never attempted.
         * Appended after the pure virtuals, so the existing slot order is kept.
         */
        virtual bool StoreReadable() { return true; }

       protected:
        ~ViewSource() = default;
      };

      /**
       * The lifecycle of the ONE active screen's view, with no LVGL object.
       *
       * Owns the view's storage: heap-allocated by Activate() (one block of
       * sizeof(ActiveScreenView)), freed by Deactivate(). EVERY materialization
       * (activation, the commit redraw, a swipe, the wake reset) holds the heap phase
       * (Idle -> Activation; 3b.1 revision 3) from before the store read until the caller's
       * render is done. A call that returns Ready, NoLayout or NoMemory returns with the phase
       * HELD, and the caller ends the hold with EndMaterialization() once it has built and
       * drawn its widgets; a first chunk is refused meanwhile. Deferred, Unchanged and
       * Inactive hold nothing.
       * A failed exchange, an odd generation, a generation that moved during the read, or a
       * transient read error defers WITHOUT dropping: the request is kept (HasPending()), the
       * update signal and the view are left as they were, and the caller retries from its
       * 1000 ms refresh with Refresh(). While deferred, view() keeps the last materialized
       * screen.
       *
       * Every call is made on the display task.
       */
      class ActiveScreenModel {
       public:
        enum class Outcome : uint8_t {
          Unchanged,  // Refresh: no pending request and no update signal
          Ready,      // view() holds the requested screen of the committed layout; phase HELD
          NoLayout,   // the store holds no valid record: view() is empty (version 0), draw the default group; phase HELD
          Deferred,   // the phase is held, the generation is odd or moved, or a read failed: request kept, retry
          NoMemory,   // the view's storage could not be allocated: draw the safe face; retried; phase HELD
          Inactive    // Activate() has not been called, or Deactivate() has
        };

        /** Consecutive transient read errors tolerated (the view kept) before the record counts as absent. */
        static constexpr uint8_t kMaxReadRetries = 3;

        explicit ActiveScreenModel(ViewSource& source)
          : source_(source), active_(false), pending_(false), readRetries_(0) {
        }
        ~ActiveScreenModel() {
          Deactivate();
        }
        ActiveScreenModel(const ActiveScreenModel&) = delete;
        ActiveScreenModel& operator=(const ActiveScreenModel&) = delete;

        /**
         * The HA face activated: allocate the view and materialize `screen` (clamped to the
         * layout's screen count). Takes the per-activation allocation baseline.
         */
        Outcome Activate(uint8_t screen = 0);

        /**
         * Activate without reading: the face is active and owes the read of `screen`, which the
         * next Refresh() serves. Holds and allocates nothing.
         */
        Outcome ActivateDeferred(uint8_t screen = 0);

        /** The HA face deactivated: free the view, drop any pending request. */
        void Deactivate();

        /** A swipe or the wake reset: materialize `screen`. */
        Outcome SwitchTo(uint8_t screen) {
          return Materialize(screen);
        }

        /**
         * The 1000 ms refresh. A pending request is retried; else a raised update signal is
         * served (the commit redraw) for the screen shown now.
         */
        Outcome Refresh();

        /**
         * End the hold a materialization left on the heap phase (Ready, NoLayout, NoMemory): call
         * it once the widgets are built and drawn. Releases only an Activation hold, so it never
         * frees a transfer's; calling it with nothing held does nothing.
         */
        void EndMaterialization();

        /** Consecutive read errors of the current request, for telemetry (0..kMaxReadRetries). */
        uint8_t ReadRetries() const {
          return readRetries_;
        }

        /** The view, or null when inactive or its storage could not be allocated. */
        const ActiveScreenView* view() const {
          return view_;
        }
        bool active() const {
          return active_;
        }
        /** The screen index the view holds (after clamping). */
        uint8_t screen() const {
          return screen_;
        }
        /** A request was deferred and is still owed (deferred, never dropped). */
        bool HasPending() const {
          return pending_;
        }
        /** The screen a pending request wants. */
        uint8_t PendingScreen() const {
          return wantScreen_;
        }

        /** Cumulative view-allocation failures, for telemetry. Never reset. */
        uint32_t AllocFailures() const {
          return allocFailures_;
        }
        /** Failures since the last Activate(): the baseline comparison, so a failure does not latch. */
        uint32_t AllocFailuresThisActivation() const {
          return static_cast<uint16_t>(allocFailures_ - allocBaseline_);
        }

      private:
        Outcome Materialize(uint8_t screen);
        Outcome MaterializeLocked(uint8_t screen);
        Outcome Defer(uint8_t screen);
        void ClearView();

        ViewSource& source_;
        ActiveScreenView* view_ {nullptr};
        // The two flags and the retry count share one byte: no new resident bytes.
        bool active_ : 1;
        bool pending_ : 1;
        uint8_t readRetries_ : 6;
        uint8_t screen_ {0};
        uint8_t wantScreen_ {0};
        // Telemetry counters: 16 bits wrap harmlessly, and the per-activation figure is a difference.
        uint16_t allocFailures_ {0};
        uint16_t allocBaseline_ {0};
      };

    } // namespace HaBridge
  }   // namespace Applications
} // namespace Pinetime
