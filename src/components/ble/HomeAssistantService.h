#pragma once

#include <cstdint>
#include <cstring>
#include <array>
#include <tuple>
#include "../../ha_bridge/DashboardLimits.h"
#include "../../ha_bridge/DashboardState.h"
#include "../../ha_bridge/PhoneNotifySync.h"
#include "../../ha_bridge/WakeTelemetry.h"
#include <atomic>
#include <vector>
#include <functional>

#define min
#define max
#include <host/ble_gap.h>
#include <host/ble_uuid.h>
#undef max
#undef min


#include <FreeRTOS.h>
#include <semphr.h>
#include <timers.h>
namespace Pinetime {
namespace System { class SystemTask; }
namespace Controllers {

class FS;
class NotificationManager;

class HomeAssistantService {
public:
  // An entity state the face's tiles read, keyed by EntityKey(entityId): the tiles are the
  // table's only reader, and a 4-byte key in place of the 64-character ID lets it hold a
  // state for every tile of a layout in less RAM than the 6 full-text entries it had.
  struct Entity {
    uint32_t key {0};
    std::array<char, 32> state{};
  };

  // Full WatchFaceLayout
  struct WatchTile {
    std::array<char, 20> label{};
    std::array<char, 16> icon{};   // Icon identifier (e.g., "garage", "light", "door")
    std::array<char, 64> entityId{};
    std::array<char, 8> color{};
    bool colorEnabled {false};  // ON/OFF for dynamic entity-state coloring (zero-default keeps BSS)
    bool confirm {false};     // Show confirmation dialog before toggling (locks, covers)
    uint8_t displayMode {0};  // 0 = text label (default), 1 = icon glyph
  };

  // Dashboard structs for read-only status display
  struct CalendarEvent {
    std::array<char, 8> time{};
    std::array<char, 32> title{};
    uint32_t eventStartEpoch {0};  // UTC epoch seconds; 0 = not provided (old Android)
  };

  struct StatusGlyph {
    char symbol {'\0'};  // Zero-default keeps BSS; set by layout push
    bool active {false};
  };

  // FR-4a — 5 status groups. The card, the HA integration and
  // DashboardLimits::MAX_STATUS_GROUPS have all allowed 5 since v5; this
  // storage cap was left at 2, so groups 3-5 were accepted upstream, sent over
  // BLE, and silently dropped on decode. Cost is 3 x sizeof(StatusGroup)
  // (37 B) = 111 B of .bss, comfortably inside the headroom freed by removing
  // the unreachable BLE mailbox.
  static constexpr uint8_t kMaxStatusGroups = 5;
  static constexpr uint8_t kMaxGlyphsPerGroup = 10;

  struct StatusGroup {
    std::array<StatusGlyph, kMaxGlyphsPerGroup> glyphs{};
    uint8_t glyphCount {0};
  };

  struct SensorGroup {
    std::array<int16_t, 7> values{};
    uint8_t valueCount {0};
  };

  // These storage caps are hand-written here but are ALSO published to the HA
  // card, the integration and Android through scripts/config/firmware-limits.yaml
  // -> DashboardLimits.h. When the two disagree the upstream side happily
  // accepts configurations this struct cannot hold and the surplus is dropped
  // silently on decode — which is exactly how status groups 3-5 went missing.
  // Bind them together so a future divergence is a build failure, not a
  // silently truncated dashboard.
  static_assert(kMaxStatusGroups == Pinetime::Applications::HaBridge::DashboardLimits::MAX_STATUS_GROUPS,
                "kMaxStatusGroups must match firmware-limits.yaml max_status_groups");
  static_assert(kMaxGlyphsPerGroup == Pinetime::Applications::HaBridge::DashboardLimits::MAX_GLYPHS_PER_GROUP,
                "kMaxGlyphsPerGroup must match firmware-limits.yaml max_glyphs_per_group");
  static_assert(std::tuple_size<decltype(SensorGroup::values)>::value
                    == Pinetime::Applications::HaBridge::DashboardLimits::MAX_SENSORS_PER_GROUP,
                "SensorGroup::values must match firmware-limits.yaml max_sensors_per_group");

  // LogbookEntry: compact entry for Logbook app display.
  // Abbreviated state keeps the struct small. All fields zero-init for BSS.
  struct LogbookEntry {
    std::array<char, 8>  time{};     // "HH:MM"
    std::array<char, 8>  state{};    // Abbreviated: "CLSD","OPN","ON","OFF","LCKD", etc.
    std::array<char, 32> subject{};  // Entity friendly name / description
  };

  // CalendarAppEntry: one event for the full-day Calendar app view.
  // Separate from Dashboard::CalendarEvent which is the mini dashboard view.
  struct CalendarAppEntry {
    std::array<char, 6>  time{};     // "HH:MM"
    std::array<char, 48> title{};    // Event title
  };

  // HomeControlDevice: one controllable device for the HaHomeControl app.
  // All fields zero-init for BSS. No icon or colour: the phone sends neither ("l", "e", "s", "a").
  struct HomeControlDevice {
    std::array<char, 20> label{};     // Friendly name "Living Room"
    std::array<char, 64> entityId{};  // "light.living_room"
    std::array<char, 16> state{};     // Current state: "on", "off", "open", "closed", etc.
    bool active {false};              // true = entity is in active state (on/open/etc.)
  };

  static constexpr uint8_t kMaxHomeControlDevices = 4;

  // Not stored: the dashboard notifications, the weather temperature and the
  // status/sensor group type. No screen renders them, and the decoder skips
  // their keys (LayoutDecoder.cpp), which keeps this target, and the stack
  // frame that holds it during a decode, smaller.
  struct Dashboard {
    // Calendar events
    std::array<CalendarEvent, 2> events{};
    uint8_t eventCount {0};

    // Weather
    std::array<char, 16> weatherCondition{};
    int8_t weatherHi {0};
    int8_t weatherLo {0};

    // Status groups (lights, doors, etc.)
    std::array<StatusGroup, kMaxStatusGroups> statusGroups{};
    uint8_t statusGroupCount {0};

    // Sensor groups (temps, presence, etc.)
    std::array<SensorGroup, 3> sensorGroups{};
    uint8_t sensorGroupCount {0};

    // Configurable headline value (v5): one HA entity displayed prominently
    struct ConfigurableValue {
      std::array<char, 12> label {};
      std::array<char, 20> value {};
      bool hasValue {false};
    };
    ConfigurableValue configurableValue {};

    // Inline metric slots (v5): 2 optional metrics flanking the clock
    struct InlineMetric {
      std::array<char, 3> label {};   // 2-char + null; empty = no label rendered
      std::array<char, 4> value {};   // 3-char + null
      bool enabled {false};
    };
    std::array<InlineMetric, 2> inlineMetrics {};

    // Epoch seconds when dashboard data was last enriched by Android bridge.
    // Zero-default: unknown/not-yet-enriched. Decoded from CBOR key "lea".
    uint32_t lastEnrichedAt {0};
  };

  // Layout capacity constants — match real UI constraints to minimize RAM.
  // Only 2 screen types supported: dashboard (0) and tile (1), max 6 screens, max 4 tiles (2x2).
  // Actions removed in v2 — tiles are the interactive elements.
  //
  // Multi-screen restore (4 screens / 4 tiles; the cap is 6 screens since 2.4.100, and the face
  // holds only the active screen, so the cap costs the entity table and nothing else): screens[]
  // scales by the tile array + 3 scalars only
  // (Dashboard is hoisted to layout level, see :208-210), so 4/4 adds ~+1560 B of static .bss
  // (sizeof(WatchFaceLayout) ~912 -> ~2475 B). That figure was the resident layout copy, which the
  // per-screen view (ha_bridge/ActiveScreenView.h) has since replaced; the layout struct is now
  // only the decode target.
  // BINDING CONSTRAINT is the RUNTIME safe-face floor (xPortGetFreeHeapSize >= kFreeHeapSafeThresholdBytes),
  // NOT the FR-A2 __HeapLimit ceiling: at 4/4 the dashboard-up free heap drops to ~3900 B (~828 B above
  // the 3072 floor), while FR-A2 still has ~2870 B of static headroom. DO NOT raise any other cap toward
  // its original target without re-running the runtime-margin math — a flat restore (+~3100 B) fits the
  // static ceiling but busts the runtime floor (this is the REQ-009 allocate-on-use trap). See
  // docs/Plans/_archive/ha-multiscreen-swipe-plan.md.
  static constexpr uint8_t kMaxScreens = 6;
  static constexpr uint8_t kMaxTilesPerScreen = 4;

  struct WatchScreen {
    uint8_t screenType {0};
    uint8_t tileLayout {0};  // 0 = default (2x2), 4 = 2x2 (zero-default keeps BSS)
    std::array<WatchTile, kMaxTilesPerScreen> tiles{};
    uint8_t tileCount {0};
    // Actions removed in v2 — tiles are the interactive elements.
  };

  // Alias for backwards compatibility with UI code
  using WatchFaceScreen = WatchScreen;

  struct WatchFaceLayout {
    uint8_t version {0};
    std::array<char, 16> timeFormat{};
    std::array<WatchScreen, kMaxScreens> screens{};
    uint8_t screenCount {0};
    // Global colors for entity-state dynamic coloring (ON/OFF per tile)
    std::array<char, 8> defaultActiveColor{};    // "#FFD700" (gold)
    std::array<char, 8> defaultInactiveColor{};  // "#800080" (purple)
    // Dashboard stored once at layout level (only one dashboard screen supported).
    // Moved out of WatchScreen to avoid paying ~350 bytes per screen.
    Dashboard dashboard{};
    bool hasDashboard {false};
    uint8_t dashboardScreenIndex {0};
  };

  HomeAssistantService();
  void Init();

  int OnCommand(struct ble_gatt_access_ctxt* ctxt);
  void SetConnectionHandle(uint16_t connHandle);
  // Whether the current link is encrypted (2.4.101): the "enc" the HELLO and every PONG report, so
  // the phone sends notification content only over an encrypted link. False with no link.
  bool IsLinkEncrypted() const;

  // Callback types
  using ActionCallback = std::function<void(uint16_t actionId, const char* gesture)>;
  using FrameCallback = std::function<void(const uint8_t* data, size_t len)>;
  using TickCallback = std::function<void()>;

  void SetOnActionInvoked(ActionCallback cb) { onActionInvoked_ = std::move(cb); }
  void SetOnFrameReady(FrameCallback cb) { onFrameReady_ = std::move(cb); }
  void SetOnTick(TickCallback cb) { onTick_ = std::move(cb); }

  // Called by UI screen's Refresh() to drive BridgeController::OnTick()
  void ProcessTick() { if (onTick_) onTick_(); }

  // Called by UI when user taps action
  void InvokeAction(uint16_t actionId, const char* gesture);

  // Pending tile entity for tile-tap actions (set before InvokeAction, read by BridgeController)
  void SetPendingTileEntity(const char* entityId);
  const char* GetPendingTileEntity() const { return pendingTileEntity_.data(); }
  void ClearPendingTileEntity() { pendingTileEntity_[0] = '\0'; }

  // A pushed layout is written to the flash store by the layout writer
  // (ha_bridge/LayoutCommit.h); no copy of it is resident. This is the publish
  // step after the write's directory commit is durable: the generation moves
  // to a new even value, and only then is the update signal raised, so a reader
  // that sees the signal sees the new generation. The caller issues __DMB()
  // before calling. Publish order: data (the commit), generation, signal.
  void PublishLayoutCommitted();

  // Heap phase (ha_bridge/LayoutCommit.h: HeapPhase, TryEnterHeapPhase,
  // ReleaseHeapPhase). One byte, Idle = 0, Transfer = 1, Activation = 2. A
  // WATCHFACE_LAYOUT transfer holds Transfer from its first chunk until the
  // writer has freed the buffer; every display-side materialization takes
  // Activation the same way and stores Idle when its render is done.
  std::atomic<uint8_t>& HeapPhaseByte() { return heapPhase_; }
  uint8_t GetHeapPhase() const { return heapPhase_.load(std::memory_order_acquire); }

  // The file system the layout store lives on. Bound once at boot; null until then.
  void BindFileSystem(FS& fs) { fs_ = &fs; }
  FS* FileSystem() const { return fs_; }

  // The layout write holds a system wake lock through it: a sleeping watch has
  // the SPI bus off (and, in always-on display, the flash powered down), and a
  // store call then spins forever on the bus. The face reads the store only
  // while SystemAwake() reports Running. Bound in NimbleController::Init next
  // to the file system; unbound (host tests) the wake calls do nothing and the
  // system counts as awake.
  void BindSystemTask(Pinetime::System::SystemTask& systemTask);
  void RequestWake();   // posts System::Messages::DisableSleeping
  void ReleaseWake();   // posts System::Messages::EnableSleeping
  void WakeScreen();    // posts System::Messages::GoToRunning: no lock, the usual timeout
  // Ends the early sleep after a notification wake that may be running, and keeps it from
  // starting for the next forMs milliseconds (0: only ends it). Callable from any task.
  void SuspendEarlySleep(uint32_t forMs);
  bool SystemAwake() const;

  // The screen-wake telemetry for the HELLO (2.4.104): the counters since boot and the settings
  // that drive the battery. Callable from any task.
  Pinetime::Applications::HaBridge::WakeDiag ReadWakeDiag() const;

  bool HasUpdate() const { return hasUpdate_.load(std::memory_order_acquire); }

  // REQ-013 — atomic test-and-clear of the layout-update flag. Replaces the
  // separate HasUpdate()/ClearUpdate() pair that allowed an incoming BLE
  // update to be lost between the load and the store. Use TakeUpdate() at
  // every check-then-clear site; reserve HasUpdate() for read-only checks.
  bool TakeUpdate() { return hasUpdate_.exchange(false, std::memory_order_acq_rel); }

  // Data freshness flags — set by BridgeController when STATE_UPDATE carries
  // ha_offline or data_stale flags, cleared when a clean update arrives.
  void SetDataStale(bool stale) { dataStale_.store(stale, std::memory_order_release); }
  bool IsDataStale() const { return dataStale_.load(std::memory_order_acquire); }
  // Seconds since the phone last wrote anything (it pings every 30 s while connected);
  // since boot until the first write. The stale banner's "No phone" age.
  uint32_t SecondsSinceLastRx() const;

  // Layout generation: odd = a write is publishing, even = stable. The display task
  // reads the layout only through ha_bridge/ActiveScreenView.h (ActiveScreenModel),
  // which captures this before its store read, defers on odd and discards the read if
  // it moved.
  uint32_t GetLayoutGen() const { return layoutGen_.load(std::memory_order_acquire); }

  // Entity states: a fixed static array, mutex-protected (the BLE task writes through
  // UpdateEntity, the display task reads through GetEntityState).
  // Until 2.4.94 it held 6 entries of 144 B (ID, state, name, unit), filled first with the
  // CONFIG_PUSH entities, which no screen shows: with more than 6 entities the app's tile
  // states after the sixth were dropped and those tiles read "---". 16 entries of 36 B hold
  // every tile of a 4-screen layout in 288 B less. 2.4.100 raised the cap to 24 entries (864 B) for
  // the 6 screens of 4 tiles (+288 B).
  static constexpr uint8_t kMaxEntities = 24;
  static constexpr uint8_t kMaxLogbookEntries = 8;
  static constexpr uint8_t kMaxCalendarEntries = 5;

  // FNV-1a of the entity ID: the table's key.
  static uint32_t EntityKey(const char* entityId);
  uint8_t GetEntityCount() const;
  Entity GetEntity(uint8_t index) const;  // Returns by value (mutex-safe copy)
  // The state of `entityId` into `out` (always terminated); false, with `out` empty, when
  // the table has none.
  bool GetEntityState(const char* entityId, char* out, size_t outLen) const;

  // Upsert an entity state: updates the entry with this ID in place, or adds one while
  // there is room. Returns false on mutex timeout.
  bool UpdateEntity(const char* entityId, const char* state);
  // Empties the table at the next UpdateEntity. A new layout calls it: the app follows the
  // layout with its tiles' states, which then replace the previous layout's. It waits for
  // nothing (it runs in the GATT write callback), and until the states come the tiles keep
  // the ones they show.
  void ResetEntitiesOnNextUpdate() { entitiesResetPending_.store(true, std::memory_order_release); }

  // Notifications are InfiniTime's own (2.4.95): a NOTIFY shows as the popup with the
  // vibration (under the watch's do-not-disturb setting) and joins the swipe-down list,
  // like an alert from the Alert Notification Service. Until 2.4.95 they went to a list of
  // 3 that only the HA Notifications app showed, with no popup and no vibration.
  // Bound in NimbleController's constructor; unbound (host tests) PushNotification drops it.
  void BindNotificationManager(NotificationManager& notificationManager) {
    notificationManager_ = &notificationManager;
    notifySync_.Bind(notificationManager, &HomeAssistantService::OnNotifyDismissed, this);
  }
  // Pushes "title\0body" (cut to the manager's 100 characters) and announces it to SystemTask.
  // 2.4.104: with the screen off, a notification that comes less than 60 s after the last one that
  // woke it is only stored (no wake, no buzz, no pop-up); and a screen a notification woke is put
  // back to sleep 5 s after the pop-up closes unless the user touches the watch (WakeTelemetry.h).
  // Runs in the GATT write callback, as the Alert Notification Service does.
  // nid (2.4.101) is the phone's id for a phone notification, 0 for none: an entry with the same
  // nid is replaced, and the user dismissing it on the watch is reported through the callback below.
  void PushNotification(const char* title, const char* body, uint16_t nid = 0);
  // NOTIFY_REMOVE: takes the entry of `nid` out of the list, silently. False when there is none.
  bool RemoveNotification(uint16_t nid) { return notifySync_.Remove(nid); }
  // Called on the display task, with no lock held, when the user dismissed a phone notification
  // (one pushed with a nid). The bridge sends NOTIFY_DISMISS from it.
  using NotifyDismissCallback = std::function<void(uint16_t nid)>;
  void SetOnNotifyDismissed(NotifyDismissCallback cb) { onNotifyDismissed_ = std::move(cb); }

  // Logbook management (for HaLogbook app)
  void SetLogbookEntries(const LogbookEntry* entries, uint8_t count);
  uint8_t GetLogbookCount() const;
  const LogbookEntry* GetLogbookEntries() const;
  // Changes with every update of the entries, so an open logbook screen redraws.
  uint8_t GetLogbookGeneration() const;

  // Calendar app data
  void SetCalendarEntries(const CalendarAppEntry* entries, uint8_t count, int8_t dayOffset);
  uint8_t GetCalendarCount() const;
  const CalendarAppEntry* GetCalendarEntries() const;
  int8_t GetCalendarDayOffset() const;
  void SetCalendarDayOffset(int8_t offset);  // Called when user navigates
  // Changes with every update of the entries or the day, so an open calendar screen redraws.
  uint8_t GetCalendarGeneration() const;

  // The dashboard's live state (DASHBOARD_STATE, 2.4.96; ha_bridge/DashboardState.h). The BLE
  // task publishes a whole decoded state; the display task copies it. An odd/even generation,
  // as the layout's: odd while a state is written. Zero until the first state since boot.
  void PublishDashLive(const Pinetime::Applications::HaBridge::DashLive& state);
  // A consistent copy of the latest state, and (when asked) the generation it is; false when
  // there is none yet, or a write kept overlapping the copy (`out` may then hold a mix: the face
  // draws nothing from it and tries again on its next refresh).
  bool ReadDashLive(Pinetime::Applications::HaBridge::DashLive& out, uint32_t* generation = nullptr) const;
  uint32_t GetDashLiveGeneration() const { return dashLiveGen_.load(std::memory_order_acquire); }
  // True from a navigation until the phone's entries for that day arrive.
  bool IsCalendarNavPending() const;

  // Home control devices (for HaHomeControl app)
  void SetHomeControlDevices(const HomeControlDevice* devices, uint8_t count);
  uint8_t GetHomeControlCount() const;
  const HomeControlDevice* GetHomeControlDevices() const;
  // Changes with every update of the devices, so an open home-control screen redraws.
  uint8_t GetHomeControlGeneration() const;

  // BLE communication callbacks
  using DataCallback = std::function<void(const uint8_t* data, size_t len)>;
  void SetOnDataReceived(DataCallback cb) { onDataReceived_ = std::move(cb); }

  // Send BLE notification
  // Returns: true if sent successfully, false if mbuf allocation or send failed
  bool SendNotify(const uint8_t* data, size_t len);

  // Get mbuf failure count for diagnostics
  uint8_t GetMbufFailures() const { return mbufFailures_; }
  /** ble_gattc_notify_custom rc != 0 count, and the most recent rc. */
  uint8_t GetNotifyFailures() const { return notifyFailures_; }
  int16_t GetLastNotifyError() const { return lastNotifyError_; }

  // REQ-010 — count of OnCommand writes that exceeded the OnCommand stack
  // buffer and were therefore rejected (NOT silently truncated). Exposed in
  // HELLO so the phone can detect a protocol mismatch instead of inheriting
  // bizarre layout state.
  uint32_t GetTruncatedWriteFailures() const {
    return truncatedWriteFailures_.load(std::memory_order_relaxed);
  }

  // REQ-030 — count of lv_*_create() calls that returned null on the HA
  // screen path. Driven by HomeAssistant::LvObjOrNull. REQ-064 reads this
  // (along with lv_mem_monitor.free_biggest_size) to trip safe-face mode.
  // HELLO field exposure lands with REQ-056 (Phase F).
  void IncrementLvCreateFailures() {
    lvCreateFailures_.fetch_add(1, std::memory_order_relaxed);
  }
  uint16_t GetLvCreateFailures() const {
    return lvCreateFailures_.load(std::memory_order_relaxed);
  }

  // REQ-064 — safe-face mode trip counter. Incremented once per false→true
  // transition of safeFaceMode_ in HomeAssistant. Exposed via HELLO so the
  // phone can surface "layout too heavy" diagnostics (REQ-067).
  void IncrementSafeFaceTrips() {
    safeFaceTrips_.fetch_add(1, std::memory_order_relaxed);
  }
  uint16_t GetSafeFaceTrips() const {
    return safeFaceTrips_.load(std::memory_order_relaxed);
  }

  // DASHBOARD_STATE health since boot, in every SENSOR_REPORT (ha_bridge/SensorReport.h), so a
  // bundle can tell states that never arrive from states refused and from states drawn against
  // another layout. PublishDashLive counts dsOk and stamps dsAge. 16-bit counts, wrapping.
  // dsBad: a DASHBOARD_STATE the decoder refused (BLE task, HandleDashboardState).
  void CountDashStateRefused() {
    dashStateRefused_.fetch_add(1, std::memory_order_relaxed);
  }
  // fpMis: the display task drew a live state whose fingerprint does not match the layout.
  // Counted once per state (its generation): the face draws the same state again every minute
  // and on every visit, and a per-render count would measure how long it was looked at.
  void CountFingerprintMiss(uint32_t generation) {
    if (fingerprintMissGen_.exchange(generation, std::memory_order_relaxed) != generation) {
      fingerprintMisses_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  uint16_t GetDashStateOk() const { return dashStateOk_.load(std::memory_order_relaxed); }
  uint16_t GetDashStateRefused() const { return dashStateRefused_.load(std::memory_order_relaxed); }
  uint16_t GetFingerprintMisses() const { return fingerprintMisses_.load(std::memory_order_relaxed); }
  // dsAge: seconds since the last DASHBOARD_STATE was published; 0 when none since boot.
  uint32_t SecondsSinceDashState() const;

  // REQ-068 — pending action tile index.  Set by the display task (OnTileTapped)
  // so BridgeController::OnActionInvoked can record which tile triggered the
  // ACTION_INVOKE and later route ACTION_RESULT back to the correct tile.
  void SetPendingActionTileIndex(uint8_t idx) {
    pendingActionTileIndex_.store(idx, std::memory_order_release);
  }
  uint8_t GetPendingActionTileIndex() const {
    return pendingActionTileIndex_.load(std::memory_order_acquire);
  }

  // REQ-068 — action result cross-task signal.
  //   resultCode: 0 = none, 1 = pending (⌛), 2 = ok (✓), 3 = fail (✗)
  //   tileIndex : which tile to overlay
  // Set by BridgeController (BLE task) on ACTION_RESULT arrival; cleared by
  // the display task after the 2-second overlay expires.
  void SetActionResult(uint8_t resultCode, uint8_t tileIndex) {
    actionResultTileIndex_.store(tileIndex, std::memory_order_relaxed);
    // Write result code last with release so the display task always reads a
    // consistent tileIndex when it observes a non-zero code.
    actionResultCode_.store(resultCode, std::memory_order_release);
  }
  // Returns the current result code (0 = none) and fills tileIndex by reference.
  uint8_t GetActionResult(uint8_t& tileIndex) const {
    uint8_t code = actionResultCode_.load(std::memory_order_acquire);
    tileIndex     = actionResultTileIndex_.load(std::memory_order_relaxed);
    return code;
  }
  void ClearActionResult() {
    actionResultCode_.store(0, std::memory_order_release);
  }

  // REQ-070 — calendar event start vibration.
  // Called by HomeAssistant display task when a calendar event start epoch
  // is crossed. BridgeController::OnTick drains this via TakeCalendarVibration.
  void RequestCalendarVibration() {
    calendarVibratePending_.store(true, std::memory_order_release);
  }
  // Returns true and clears the flag if a vibration was requested; false otherwise.
  bool TakeCalendarVibration() {
    bool expected = true;
    return calendarVibratePending_.compare_exchange_strong(
        expected, false, std::memory_order_acq_rel, std::memory_order_relaxed);
  }

  // Get current MTU for dynamic renegotiation handling
  // WHY: BLE connections can renegotiate MTU after connection establishment.
  // Returns 0 if not connected, otherwise returns current MTU in range [23, 517].
  // Default fallback is 247 if query fails.
  uint16_t GetCurrentMtu() const;

private:
  // 128-bit UUIDs matching Android side and InfiniTime base UUID pattern
  // UUIDs stored in little-endian byte order
  // Service: 00060000-78fc-48fe-8e23-433b3a1942d0
  // TX Char: 00060001-78fc-48fe-8e23-433b3a1942d0
  // RX Char: 00060002-78fc-48fe-8e23-433b3a1942d0
  static constexpr ble_uuid128_t haServiceUuid {
    .u = {.type = BLE_UUID_TYPE_128},
    .value = {0xd0, 0x42, 0x19, 0x3a, 0x3b, 0x43, 0x23, 0x8e,
              0xfe, 0x48, 0xfc, 0x78, 0x00, 0x00, 0x06, 0x00}
  };
  static constexpr ble_uuid128_t haTxCharUuid {
    .u = {.type = BLE_UUID_TYPE_128},
    .value = {0xd0, 0x42, 0x19, 0x3a, 0x3b, 0x43, 0x23, 0x8e,
              0xfe, 0x48, 0xfc, 0x78, 0x01, 0x00, 0x06, 0x00}
  };
  static constexpr ble_uuid128_t haRxCharUuid {
    .u = {.type = BLE_UUID_TYPE_128},
    .value = {0xd0, 0x42, 0x19, 0x3a, 0x3b, 0x43, 0x23, 0x8e,
              0xfe, 0x48, 0xfc, 0x78, 0x02, 0x00, 0x06, 0x00}
  };

  // CRITICAL: txHandle must be declared BEFORE characteristicDefinition
  // because the constructor takes &txHandle when initializing the array
  uint16_t txHandle {0};
  uint16_t connHandle {BLE_HS_CONN_HANDLE_NONE};

  struct ble_gatt_chr_def characteristicDefinition[3];
  struct ble_gatt_svc_def serviceDefinition[2];

  // Data storage
  std::array<char, 64> pendingTileEntity_ {};

  // Callbacks
  ActionCallback onActionInvoked_;
  FrameCallback onFrameReady_;
  TickCallback onTick_;

  std::atomic<bool> hasUpdate_ {false};
  std::atomic<bool> dataStale_ {false}; // true when ha_offline or data_stale flag received
  std::atomic<uint32_t> lastRxTick_ {0}; // xTaskGetTickCount() at the phone's last write
  std::atomic<uint32_t> layoutGen_ {0}; // Seqlock: odd=writing, even=stable (data race guard)
  std::atomic<uint32_t> dashLiveGen_ {0}; // Seqlock over dashLive_: odd=writing, 0=none yet
  std::atomic<uint8_t> heapPhase_ {0};  // HeapPhase::Idle
  FS* fs_ {nullptr};
  Pinetime::System::SystemTask* systemTask_ {nullptr};
  mutable SemaphoreHandle_t dataMutex_ {nullptr};  // Protects entity access

  // Entities — STATIC array (BSS). Moved from heap to eliminate boot-time new() call
  // that could crash with -fno-exceptions on OOM (boot failure fix v1.172).
  // 10 × 144 = 1,440 bytes in BSS. Combined with reassembly buffer shrink (-2KB),
  // net BSS delta is -608 bytes.
  static Entity entities_[kMaxEntities];
  uint8_t entityCount_ {0};
  std::atomic<bool> entitiesResetPending_ {false};

  NotificationManager* notificationManager_ {nullptr};
  Pinetime::Applications::HaBridge::PhoneNotifySync notifySync_;

  // The early sleep after a notification wake (2.4.104). The gate runs in PushNotification (the
  // GATT write callback); the watch is armed there and polled by a one-shot timer that re-arms
  // itself on the timer daemon until it stops. Both are guarded by a critical section.
  Pinetime::Applications::HaBridge::NotifyWakeGate notifyGate_;
  Pinetime::Applications::HaBridge::NotifyWakeSleepWatch sleepWatch_;
  TimerHandle_t notifyWakeTimer_ {nullptr};
  void PollNotifyWake();
  static void NotifyWakeTimerCb(TimerHandle_t timer);
  NotifyDismissCallback onNotifyDismissed_;
  static void OnNotifyDismissed(void* self, uint16_t nid);

  // Logbook entries (from phone, shown in HaLogbook app) — STATIC for BSS.
  // 8 × 48 = 384 bytes in BSS.
  static std::array<LogbookEntry, kMaxLogbookEntries> logbookEntries_;
  uint8_t logbookCount_ {0};
  uint8_t logbookGeneration_ {0};

  // Calendar app entries (from phone, shown in HaCalendar app) — STATIC for BSS.
  // 5 × 54 = 270 bytes in BSS.
  static std::array<CalendarAppEntry, kMaxCalendarEntries> calendarEntries_;
  // ~150 B of .bss: the live dashboard state, guarded by dashLiveGen_.
  static Pinetime::Applications::HaBridge::DashLive dashLive_;
  uint8_t calendarCount_ {0};
  int8_t calendarDayOffset_ {0};   // 0=today, +1=tomorrow, -1=yesterday
  bool calendarNavPending_ {false}; // true = watch is waiting for phone to send new events
  uint8_t calendarGeneration_ {0};

  // Home control devices (from phone, shown in HaHomeControl app) — STATIC for BSS.
  // 4 × 101 = 404 bytes in BSS.
  static std::array<HomeControlDevice, kMaxHomeControlDevices> homeControlDevices_;
  uint8_t homeControlCount_ {0};
  uint8_t homeControlGeneration_ {0};

  // BLE data callback
  DataCallback onDataReceived_;

  uint8_t mbufFailures_ {0};  // Diagnostic counter for mbuf allocation failures
  uint8_t notifyFailures_ {0};    // ble_gattc_notify_custom returned non-zero
  int16_t lastNotifyError_ {0};   // most recent non-zero rc

  // REQ-010 — increment when OnCommand receives a write larger than its stack
  // buffer. Old behaviour silently truncated; new behaviour rejects the write
  // and bumps this counter so the phone can observe protocol mismatch.
  std::atomic<uint32_t> truncatedWriteFailures_ {0};

  // REQ-030 — see IncrementLvCreateFailures for rationale.
  std::atomic<uint16_t> lvCreateFailures_ {0};

  // REQ-064 — safe-face mode trip counter; see IncrementSafeFaceTrips.
  std::atomic<uint16_t> safeFaceTrips_ {0};

  // DASHBOARD_STATE health; see CountDashStateRefused. 14 B, 16 B of .bss with alignment.
  std::atomic<uint16_t> dashStateOk_ {0};
  std::atomic<uint16_t> dashStateRefused_ {0};
  std::atomic<uint16_t> fingerprintMisses_ {0};
  std::atomic<uint32_t> fingerprintMissGen_ {0};  // the live-state generation fpMis last counted
  std::atomic<uint32_t> dashStateTick_ {0};       // xTaskGetTickCount() at the last publish

  // REQ-068 — pending action tile index: set by display task before InvokeAction;
  // consumed by BridgeController::OnActionInvoked to track which tile to overlay.
  // 0xFF = no tile pending (valid tile indices are 0..3).
  std::atomic<uint8_t> pendingActionTileIndex_ {0xFF};

  // REQ-068 — action result: written by BLE task (HandleActionResult), consumed
  // by display task (HomeAssistant::Refresh). Code: 0=none, 1=pending, 2=ok, 3=fail.
  std::atomic<uint8_t> actionResultCode_      {0};
  std::atomic<uint8_t> actionResultTileIndex_ {0xFF};

  // REQ-070 — calendar vibration request. Set by the display task when a
  // calendar event starts (epoch crossed). Consumed by BridgeController::OnTick
  // via TakeCalendarVibration() which fires the haptic and clears the flag.
  // Atomic so no mutex needed for the cross-task handoff.
  std::atomic<bool> calendarVibratePending_ {false};
};

} // namespace Controllers
} // namespace Pinetime
