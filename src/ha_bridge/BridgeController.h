#pragma once

#include "../components/ble/HomeAssistantService.h"
#include "FrameCodec.h"
#include "ProtocolMessages.h"
#include "SensorCollector.h"
#include "components/motor/MotorController.h"
#include "ha_bridge/BootConfirm.h"
#include <atomic>
#include <memory>
#include <vector>
#include <FreeRTOS.h>
#include <task.h>
#include <timers.h>

namespace Pinetime {
  namespace Controllers {
    class MotionController;
    class HeartRateController;
    class Battery;
    class DateTime;
  }
  namespace Drivers {
    class Watchdog;
  }
}

namespace Pinetime {
namespace Applications {
namespace HaBridge {

// Defined in LayoutCommit.h; only named here.
enum class StepResult : uint8_t;

// Breadcrumb codes identifying the last known processing phase before a crash.
// Stored in .noinit RAM so they survive soft resets and watchdog resets.
enum class Breadcrumb : uint8_t {
  IDLE = 0,
  BLE_RX = 1,            // Received BLE data, about to process
  FRAME_DECODE = 2,      // Decoding frame header / reassembly
  CBOR_DECODE = 3,       // Decoding CBOR payload
  CONFIG_APPLY = 4,      // Applying CONFIG_PUSH to service
  SCREEN_TRANSITION = 5, // Screen create/destroy in progress
  WATCHFACE_APPLY = 6,   // Applying WATCHFACE_LAYOUT
  SENSOR_REPORT = 7,     // Building sensor report
  BLE_TX = 8,            // Transmitting BLE notification
  CBOR_ENCODE = 9,       // Encoding CBOR response (HELLO/ACK); overflow lands here
};

// Crash context stored in .noinit RAM — survives soft resets and watchdog resets.
// On boot, BridgeController reads this and includes it in the HELLO response
// so the phone can log why the watch last reset.
//
// REQ-058: HA_CRASH_CONTEXT_EXTENDED adds ARM fault registers, task name,
// display HWM, LVGL snapshot, and layout fingerprint.
// RE-ENABLED 2.4.41. It was switched off as a precaution while the 2.4.11 boot
// loop was bisected; that loop was root-caused elsewhere (watchdog quorum — see
// HA_WATCHDOG_SUBSYSTEM_AWARE below — plus the MTU floor and safe-face
// threshold fixed in 2.4.31), so the precaution outlived its reason.
//
// Leaving it off is not free: with the gate at 0 the ARM fault handlers are
// not compiled in at all, so a hard fault lands in the weak default handler
// and pc/lr/cfsr/hfsr are never recorded anywhere. HELLO also emits only the 5
// base crashContext fields. SetBreadcrumb() still writes the magic word
// regardless of this gate, so the PHASE survives a fault — but "died somewhere
// in WATCHFACE_APPLY" does not localize a fault in a ~300-line configure path.
// The faulting address does.
//
// Cost: sizeof(CrashContext) 16 B -> 82 B in .noinit, plus ~64 B of .bss for
// BridgeController::previousCrashContext_. gcc_nrf52-mcuboot.ld places .noinit
// as (NOLOAD), so the contents survive the watchdog reboot that
// CaptureFaultContext() deliberately triggers — but NOT a power cycle, so
// recovering a wedged watch by holding the button discards the evidence.
//
// The 4-slot crash ring is NOT part of this: it costs ~1104 B and sits behind
// its own HA_CRASH_RING gate (default off) for the reasons documented there.
// 2.4.43: kept ON, but this build is a FULL RECOMPILE. 2.4.41 and 2.4.42
// both boot-looped (buzz-1, buzz-2, no buzz-3 = dies inside LoadNewScreen
// constructing the watch face) and nothing under this gate runs during boot.
// Turning the gate on changes sizeof(CrashContext) 16 -> 82, hence
// sizeof(BridgeController) and sizeof(SystemTask); every prior build was
// INCREMENTAL, so a translation unit that failed to recompile would disagree
// about those sizes — an ODR mismatch that corrupts memory and would fail
// exactly where this fails. Build clean before blaming the gate.
#ifndef HA_CRASH_CONTEXT_EXTENDED
#define HA_CRASH_CONTEXT_EXTENDED 0
#endif

// REQ-060: HA_WATCHDOG_SUBSYSTEM_AWARE must be defined BEFORE CrashContext
// because watchdogMask (field inside CrashContext) is gated on it.
// The full rationale for HA_WATCHDOG_SUBSYSTEM_AWARE lives further down where
// the enum and heartbeat array are declared; this block is only here to make
// the macro visible when the struct is parsed.
//
// EMERGENCY ROLLBACK 2.4.12 — set to 0. The original 2.4.11 design required
// all four subsystem heartbeats (Display/BLE/Sensor/System) to set before
// the watchdog would reload. The Display heartbeat is set only inside
// HomeAssistant::Refresh(), which does not run when the user is on the
// default watchface or in stock InfiniTime menus. Result: the watchdog
// quorum is never met → watchdog times out at ~1500 ms → reboot → loop.
// The 2.4.10 central-kick path is restored when this is 0; the BRD's
// subsystem-aware design needs a wiring rethink before re-enabling.
#ifndef HA_WATCHDOG_SUBSYSTEM_AWARE
#define HA_WATCHDOG_SUBSYSTEM_AWARE 0
#endif

// HA_BLE_RX_MAILBOX gates the REQ-011 BLE RX mailbox task (BleProc) and its
// wake semaphore. 0 = direct dispatch on the NimBLE host task (2.4.10
// behavior). Disabled while the 2.4.11+ boot loop is being bisected on
// hardware: the mailbox task + semaphore are boot-path deltas vs the last
// known-booting build.
#ifndef HA_BLE_RX_MAILBOX
#define HA_BLE_RX_MAILBOX 0
#endif

struct CrashContext {
  // --- Base fields (present regardless of HA_CRASH_CONTEXT_EXTENDED) ---
  uint32_t magic;              // 0xDEADCAFE = valid data present
  uint8_t  breadcrumb;         // Last Breadcrumb value before crash
  uint8_t  lastMsgType;        // Last protocol MessageType being processed
  uint16_t lastMsgId;          // Last protocol message ID
  uint32_t freeHeapAtCrash;    // xPortGetFreeHeapSize() at breadcrumb write
  uint32_t uptimeTicksAtCrash; // xTaskGetTickCount() at breadcrumb write

  // NOTE: the ARM fault registers are back under HA_CRASH_CONTEXT_EXTENDED.
  //
  // 2.4.51 promoted them to the base struct on the reasoning that 28 B of
  // .noinit was cheap. It is not, and this is now the sharpest fact we have
  // about the boot loop that has followed this project since 2.4.41 —
  // sizeof(.noinit) predicts booting exactly:
  //
  //   0x20   2.4.40, .44-.50            all boot
  //   0x3c   2.4.51, .52, .53, .54      all loop
  //   0x62   2.4.41, .42                all loop
  //
  // 2.4.54 is the controlled proof: identical to the looping 2.4.53 in bss,
  // heap and .noinit, with the fault handlers removed — and it still loops. So
  // it is not the handlers (an earlier hypothesis of mine, wrong), and it is
  // not heap size either: 2.4.41 loops with MORE heap (32688 B) than the
  // working 2.4.44-2.4.48 (32656 B).
  //
  // Growing .noinit past 0x20 is what breaks boot. Why is not yet established
  // — it is a NOLOAD section between .bss and the heap, so the suspicion is a
  // placement or alignment assumption somewhere downstream of it rather than
  // the byte count as such. Until that is understood, do NOT add fields here.
#if HA_CRASH_CONTEXT_EXTENDED
  // --- ARM Cortex-M fault registers (captured inside fault handler) ---
  // Written only from fault ISR context — no FreeRTOS primitives used.
  uint32_t pc;                 // Program Counter at fault (from exception frame)
  uint32_t lr;                 // Link Register at fault (from exception frame)
  uint32_t xpsr;               // xPSR at fault (from exception frame)
  uint32_t cfsr;               // Configurable Fault Status Register (SCB->CFSR)
  uint32_t hfsr;               // HardFault Status Register (SCB->HFSR)
  uint32_t mmfar;              // MemManage Fault Address Register (SCB->MMFAR)
  uint32_t bfar;               // BusFault Address Register (SCB->BFAR)

  // --- Task / scheduler snapshot (captured at SetBreadcrumb) ---
  char     taskName[16];       // pcTaskGetName() — truncated if longer
  uint32_t displayStackHwm;    // uxTaskGetStackHighWaterMark(displayTask) in words

  // --- LVGL pool snapshot (captured at SetBreadcrumb) ---
  uint32_t lvFreeSize;         // lv_mem_monitor_t.free_size at last breadcrumb
  uint32_t lvFreeBiggest;      // lv_mem_monitor_t.free_biggest_size at last breadcrumb

  // --- Layout fingerprint (updated on each WATCHFACE_LAYOUT apply) ---
  uint8_t  activeScreenId;     // WatchFaceLayout.screens[currentIndex].type cast to uint8_t
  uint8_t  lastResetReason;    // Watchdog::GetResetReason() numeric code at boot
  uint32_t layoutGen;          // HomeAssistantService::GetLayoutGen() seqlock counter
  uint32_t layoutCrc;          // CRC-32 of the applied layout payload (updated on apply)

#if HA_WATCHDOG_SUBSYSTEM_AWARE
  // --- Watchdog subsystem mask (REQ-060) ---
  // Bitmask of subsystems that failed to set their heartbeat before the
  // watchdog check.  Bit N = subsystem N (see WatchdogSubsystem enum).
  // Written only from task context (CheckAndKickWatchdog); read by HELLO encoder.
  uint8_t  watchdogMask;       // missing heartbeat mask at last WDT check
#endif // HA_WATCHDOG_SUBSYSTEM_AWARE
#endif // HA_CRASH_CONTEXT_EXTENDED
} __attribute__((packed));

// Compile-time byte-budget assertions (REQ-058 / REQ-060).
// Per-entry budget: ≤ 384 B.  Single g_crashContext lives alone in .noinit here;
// the 4-entry ring (REQ-059) will static_assert its own CrashRing ≤ 1544 B.
// REQ-060 adds watchdogMask (1 B) under HA_WATCHDOG_SUBSYSTEM_AWARE — still << 384.
static_assert(sizeof(CrashContext) <= 384,
              "REQ-058/REQ-060: sizeof(CrashContext) exceeds 384 B per-entry budget");

static constexpr uint32_t kCrashContextMagic = 0xDEADCAFE;

// Placed in .noinit section — declared extern here, defined in BridgeController.cpp
extern CrashContext g_crashContext __attribute__((section(".noinit")));

// ---------------------------------------------------------------------------
// REQ-060 — subsystem-aware watchdog.
//
// When HA_WATCHDOG_SUBSYSTEM_AWARE is ON (default), each subsystem must set
// its heartbeat flag before the watchdog is kicked.  If any flag is missing
// when the check fires, the watchdog is NOT reloaded and the missing-mask
// bitmask is recorded in g_crashContext for post-mortem attribution.
//
// When HA_WATCHDOG_SUBSYSTEM_AWARE is OFF, the old single-source kick is
// used — BridgeController::OnTick calls watchdog_->Reload() unconditionally.
// Flip to 0 at compile time for rollback to the 2.4.10 central-kick path.
//
// Deadlines (all relative to watchdog timeout; display 1500ms, NimBLE 2000ms,
// sensor 5000ms, system 1000ms — enforced by the host WDT timeout, not by
// this code which merely gates the kick).
//
// ISR-safety: each entry in g_watchdogHeartbeat is a volatile uint8_t.
// From ISR context only plain byte-write `g_watchdogHeartbeat[i] = 1` is
// allowed — no FreeRTOS, no mutex, no printf, no malloc.
// From task context, CheckAndKickWatchdog() reads and clears all entries
// atomically under a critical section before calling Reload().
// ---------------------------------------------------------------------------
#if HA_WATCHDOG_SUBSYSTEM_AWARE

// Subsystem index constants — index into g_watchdogHeartbeat[].
enum class WatchdogSubsystem : uint8_t {
  Display = 0,   // Display / LVGL task (HomeAssistant::Refresh, lv_task)
  Ble     = 1,   // NimBLE host task (BleProcessingTaskLoop)
  Sensor  = 2,   // Sensor task / OnTick sensor report path
  System  = 3,   // BridgeController::OnTick system tick (always runs)
  Count   = 4
};

static constexpr uint8_t kWatchdogSubsystemCount =
  static_cast<uint8_t>(WatchdogSubsystem::Count);

// Heartbeat flags — one byte per subsystem.
// Written from any task or ISR context (byte write is atomic on Cortex-M4).
// Read + cleared only from CheckAndKickWatchdog() in task context.
extern volatile uint8_t g_watchdogHeartbeat[kWatchdogSubsystemCount]
  __attribute__((section(".noinit")));

// Set the heartbeat flag for a subsystem.  ISR-safe — single byte store.
inline void WatchdogSubsystemKick(WatchdogSubsystem id) {
  const uint8_t idx = static_cast<uint8_t>(id);
  if (idx < kWatchdogSubsystemCount) {
    g_watchdogHeartbeat[idx] = 1;
  }
}

// Mask bitmask helpers (bit N set = subsystem N is missing its heartbeat).
inline uint8_t WatchdogBuildMissingMask() {
  uint8_t mask = 0;
  for (uint8_t i = 0; i < kWatchdogSubsystemCount; ++i) {
    if (g_watchdogHeartbeat[i] == 0) {
      mask |= static_cast<uint8_t>(1u << i);
    }
  }
  return mask;
}

#endif // HA_WATCHDOG_SUBSYSTEM_AWARE

// ---------------------------------------------------------------------------
// REQ-059 — 4-slot crash-dump ring in .noinit RAM.
//
// Gated SEPARATELY from HA_CRASH_CONTEXT_EXTENDED since 2.4.41, because the two
// features have very different costs and only one of them is needed to diagnose
// a fault. The ring stores crash HISTORY; the fault registers say where the
// current crash is. Measured cost of the ring on 2.4.41:
//   g_crashRing                       336 B .noinit
//   HandleCrashDumpRequest()'s static  768 B .bss
//   -------------------------------------------
//                                    1104 B
// and heap_4_infinitime.c sizes the FreeRTOS heap as
// `&__StackLimit - &__HeapLimit`, so every one of those bytes comes straight
// out of the runtime heap. With the dashboard up, measured free heap is ~4 KB
// against a 3072 B safe-face floor (kFreeHeapSafeThresholdBytes) — roughly
// 0.9 KB of headroom — so enabling the ring alone would push a rendering
// dashboard under the floor and show "layout too heavy" instead.
//
// Turn this on only alongside a matching reduction elsewhere, and re-measure
// __HeapLimit against the last known-good build before flashing.
#ifndef HA_CRASH_RING
#define HA_CRASH_RING 0
#endif

// HA_CRASH_RUNTIME_PROBES gates the parts of the extended crash context that
// execute during NORMAL operation, as opposed to inside the fault handler.
//
// 2.4.41 shipped with HA_CRASH_CONTEXT_EXTENDED=1 and boot-looped on hardware.
// The fault registers themselves are written only from CaptureFaultContext, so
// they cannot affect a healthy boot. What the gate ALSO switched on was three
// probes that run on live code paths:
//   * the pcTaskGetName() capture in SetBreadcrumb — and SetBreadcrumb runs on
//     boot and on every inbound BLE message,
//   * UpdateDisplayStackHwm() on every HomeAssistant::Refresh,
//   * UpdateCrashContextLvgl() on every layout apply.
// Those are the only new instructions on the working paths, so they are where
// a boot regression can come from; the post-mortem fields they fill are a
// nicety. Default 0: capture faults, disturb nothing while running.
//
// The fields stay in the struct and on the wire either way, reading as zero,
// so the phone needs no matching change.
#ifndef HA_CRASH_RUNTIME_PROBES
#define HA_CRASH_RUNTIME_PROBES 0
#endif

#if HA_CRASH_CONTEXT_EXTENDED && HA_CRASH_RING

static constexpr uint32_t kCrashRingMagic   = 0xC0FFEE59;  // REQ-059 sentinel
static constexpr uint8_t  kCrashRingSlots   = 4;

struct CrashRingHeader {
  uint32_t magic;      // kCrashRingMagic = valid ring
  uint8_t  head;       // write-next index (0..kCrashRingSlots-1)
  uint8_t  tail;       // read-next index  (0..kCrashRingSlots-1)
  uint8_t  count;      // number of undelivered entries (0..kCrashRingSlots)
  uint8_t  pad;        // explicit padding — keeps total header at 8 B
} __attribute__((packed));

static_assert(sizeof(CrashRingHeader) == 8,
              "REQ-059: CrashRingHeader must be exactly 8 B");

struct CrashRing {
  CrashRingHeader hdr;
  CrashContext    slots[kCrashRingSlots];
} __attribute__((packed));

// Byte-budget enforcement (REQ-059): 4 × 384 + 8 = 1544 B.
static_assert(sizeof(CrashRing) <= 1544,
              "REQ-059: sizeof(CrashRing) exceeds 1544 B budget");

// Declared extern here — defined in BridgeController.cpp as .noinit.
extern CrashRing g_crashRing __attribute__((section(".noinit")));

#endif // HA_CRASH_CONTEXT_EXTENDED && HA_CRASH_RING

// Set a breadcrumb at a processing checkpoint.
// Inline for zero call overhead in hot paths (BLE callbacks).
// Task-context only — calls xPortGetFreeHeapSize / xTaskGetTickCount / pcTaskGetName,
// none of which may be invoked from an ISR.  Fault handlers capture ARM registers
// directly into the extended fields without calling this function.
inline void SetBreadcrumb(Breadcrumb bc, uint8_t msgType = 0, uint16_t msgId = 0) {
  g_crashContext.magic              = kCrashContextMagic;
  // The high nibble is the boot step (BootConfirm::MarkStep, diagnostic builds); the
  // breadcrumbs are 0..9 and take the low one.
  g_crashContext.breadcrumb         = static_cast<uint8_t>((g_crashContext.breadcrumb & 0xF0u) |
                                                           (static_cast<uint8_t>(bc) & 0x0Fu));
#if HA_BOOT_TRACE
  // Diagnostic builds keep SystemTask's start-up stage and its tick in these two fields
  // (BootConfirm::MarkInitStage).
  (void) msgType;
  (void) msgId;
#else
  g_crashContext.lastMsgType        = msgType;
  g_crashContext.lastMsgId          = msgId;
#endif
  g_crashContext.freeHeapAtCrash    = xPortGetFreeHeapSize();
  g_crashContext.uptimeTicksAtCrash = xTaskGetTickCount();
#if HA_CRASH_CONTEXT_EXTENDED
  // Capture running task name without heap allocation.
  //
  // pcTaskGetName(nullptr) resolves to pxCurrentTCB, which is NULL until
  // vTaskStartScheduler() runs. FreeRTOS guards that with configASSERT(pxTCB),
  // and this project maps configASSERT to the nRF ASSERT handler, which RESETS
  // the chip. SetBreadcrumb IS on the boot path (BridgeController::Init ends
  // with SetBreadcrumb(IDLE)), so calling this unguarded turns every boot into
  // a reset loop — which is exactly what 2.4.41 did on hardware.
  //
  // Guard on scheduler state first, then null-check the result: a task name is
  // a nicety, and nothing here is worth risking the boot for.
  g_crashContext.taskName[0] = '\0';
#if HA_CRASH_RUNTIME_PROBES
  if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
    const char* tn = pcTaskGetName(nullptr);
    if (tn != nullptr) {
      for (size_t i = 0; i < sizeof(g_crashContext.taskName) - 1u; ++i) {
        g_crashContext.taskName[i] = tn[i];
        if (tn[i] == '\0') break;
      }
      g_crashContext.taskName[sizeof(g_crashContext.taskName) - 1u] = '\0';
    }
  }
#endif // HA_CRASH_RUNTIME_PROBES
  // LVGL pool and displayStackHwm are updated by UpdateCrashContextLvgl() which
  // is called from the display task (has access to LVGL headers) — not here.
#endif // HA_CRASH_CONTEXT_EXTENDED
}

#if HA_CRASH_CONTEXT_EXTENDED
// UpdateCrashContextLayout — called from BridgeController after each successful
// WATCHFACE_LAYOUT apply.  Records the layout fingerprint so post-mortem
// analysis knows which layout was active at fault time.
// Task-context only — call from the BLE processing task only.
inline void UpdateCrashContextLayout(uint8_t screenId, uint32_t gen, uint32_t crc) {
  g_crashContext.activeScreenId = screenId;
  g_crashContext.layoutGen      = gen;
  g_crashContext.layoutCrc      = crc;
}

// UpdateDisplayStackHwm — REQ-060 wiring of the REQ-058 reserved field.
// Called from the display task (HomeAssistant::Refresh) which has the
// displayTask handle in scope.  Uses uxTaskGetStackHighWaterMark(nullptr)
// to query the calling task's watermark — zero overhead, no handle needed.
// Task-context only; not safe from ISR.
inline void UpdateDisplayStackHwm() {
#if HA_CRASH_RUNTIME_PROBES
  // A nullptr handle means pxCurrentTCB, which FreeRTOS asserts on before the
  // scheduler starts — and configASSERT maps to the nRF ASSERT handler, which
  // resets. This is only reached from HomeAssistant::Refresh (display task, so
  // the scheduler is up), but the guard costs one comparison and removes the
  // trap for any future caller placed earlier in boot.
  if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
    return;
  }
  g_crashContext.displayStackHwm =
    static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
#endif // HA_CRASH_RUNTIME_PROBES
}
#endif // HA_CRASH_CONTEXT_EXTENDED

/**
 * BridgeController ties together the BLE service, protocol layer, and UI app.
 * It handles the bidirectional data flow:
 * - BLE → Frame decoding → CBOR decoding → UI updates
 * - UI actions → CBOR encoding → Frame encoding → BLE
 */
class BridgeController {
 public:
  // Constructor takes a reference to the existing HomeAssistantService from NimbleController
  // and references to InfiniTime controllers for sensor data collection and haptics
  BridgeController(
    Pinetime::Controllers::HomeAssistantService& bleService,
    Pinetime::Controllers::MotionController& motionController,
    Pinetime::Controllers::HeartRateController& heartRateController,
    Pinetime::Controllers::Battery& batteryController,
    Pinetime::Controllers::MotorController* motorController = nullptr,
    Pinetime::Controllers::DateTime* dateTimeController = nullptr,
    Pinetime::Drivers::Watchdog* watchdog = nullptr
  );
  ~BridgeController();

  // Initialize all components
  void Init();

  /**
   * Keep the previous boot's crash context before this boot writes to it. Called at the top of
   * SystemTask::Work, ahead of the start-up stages; Init then keeps it.
   */
  void CaptureEarly();

  // Called by UI when user invokes an action
  void OnActionInvoked(uint16_t actionId, const char* gesture);

  // The side button was double-clicked (DisplayApp, display task). When the phone has voice
  // assist switched on (the voiceAssist key of its CONFIG_PUSH, on a link that is still up)
  // this is the voice trigger: ACTION_INVOKE 0xFFFE with gesture "tap", or "double" (cancel)
  // while a voice session is active. Returns true when it did so, and the click is consumed.
  // Returns false otherwise (no link, an app without voice, voice off), and InfiniTime opens
  // Notifications as it always did.
  bool OnButtonDoubleClicked();

  // Send sensor report to phone
  void SendSensorReport();

  // Called periodically (e.g., from app OnTick or timer)
  void OnTick();

  // Send CALENDAR_NAV to phone (called from HaCalendar screen on day swipe)
  void SendCalendarNav(int8_t dayOffset);


  // OBS-1 (REQ009-FR-B5): layout-push accounting, for the HELLO diagnostics.
  // Refused = dropped at the reassembly heap guard or a failed allocation, plus
  // transfers refused because a push was pending or a materialization held the
  // heap phase or the transfer cooldown was running, plus hand-offs that could
  // not be armed. Committed / failed are
  // the writes that ended in a durable record / were aborted (previous layout kept).
  uint32_t LayoutRefusedCount() const;
  uint32_t LayoutCommittedCount() const;
  uint32_t LayoutFailedCount() const;
  // Stall watches that could not be armed (the timer daemon's queue was full);
  // those transfers rely on the disconnect clear and the next-frame timeout.
  uint32_t LayoutStallArmFailedCount() const;

  // The link dropped (BLE_GAP_EVENT_DISCONNECT). Aborts the layout transfer the
  // codec still holds: the buffer is freed once and the heap phase goes back to
  // Idle. A write already handed to the writer is not touched (the writer owns
  // its buffer and finishes on the timer daemon). Static because
  // NimbleController holds no BridgeController; a no-op before construction.
  // Must run on the task that runs FrameCodec::OnDataReceived: the NimBLE host
  // task, with HA_BLE_RX_MAILBOX 0 (BridgeController.cpp asserts it).
  static void OnBleDisconnected();

 private:
  // BLE service reference (owned by NimbleController)
  Pinetime::Controllers::HomeAssistantService& bleService_;

  // Motor controller for haptic feedback (optional, may be null)
  Pinetime::Controllers::MotorController* motorController_;

  // DateTime controller for time synchronization (optional, may be null)
  Pinetime::Controllers::DateTime* dateTimeController_;

  // Watchdog controller for reset reason (optional, may be null)
  Pinetime::Drivers::Watchdog* watchdog_;

  // Snapshot of crash context from previous boot (read in Init).
  // REQ-057: cleared only after the phone confirms it ingested the report, by
  // HELLO_ACK crashContextAck=true or by CRASH_DUMP_ACK. Until then each HELLO
  // and each CRASH_DUMP_REQUEST reply re-includes it so a dropped BLE
  // notification does not lose the forensic data.
  CrashContext previousCrashContext_ {};
  bool hasCrashContext_ {false};

  // Dashboard v5 hardening diagnostics (see FR-4, FR-6, FR-7, FR-14).
  // Declared here so Phase 3/6 writers can populate them without re-touching
  // the header.  Until those writers land, these counters stay at 0 and the
  // HELLO handler does NOT emit them (shipping them now would freeze the wire
  // names under NFR-11 while still reporting meaningless zeros).
  //
  // Populated by Phase 3 (FR-6): incremented on unknown-key skip in
  // DecodeDashboardInline, reset to 0 on a successful layout commit.
  uint16_t dashUnknownKeyCount_ {0};
  // Populated by Phase 3 (FR-4): latest DashboardDecodeStatus enum value.
  uint8_t lastLayoutDecodeStatus_ {0};
  // Populated by Phase 6 (FR-7): per-font bitfield, 1 = fallback taken.
  uint8_t fontGuardFailures_ {0};

  // Frame codec for chunking/reassembly
  FrameCodec frameCodec_;

  // The layout write path (REQ009-FR-B5): the flash store, the step machine and
  // the transfer gate the codec calls. Defined in BridgeController.cpp and placed
  // in .bss there (no heap, and the headers of the layout unit stay out of every
  // file that includes this one).
  struct LayoutPipeline;
  LayoutPipeline* layout_ {nullptr};

  // The resident one-shot step timer on the FreeRTOS timer daemon ("hlw", one
  // tick) lives in LayoutPipeline (BridgeController.cpp): the GATT callback arms
  // it at hand-off, each step re-arms it from inside the callback. Created in
  // Init(), never deleted. Its callback is LayoutPipeline::StepCb.
  //
  // noinline on both: the frame that stays on the daemon stack across a step
  // must be RunLayoutStep's small one, not the union of everything after it.
  __attribute__((noinline)) void RunLayoutStep();
  // After one step: re-arm, or publish. True when the next step must run inline.
  __attribute__((noinline)) bool FinishLayoutStep(StepResult result);
  // Before a step: hold a system wake lock for the write and wait for the system
  // to run. False when no step may run now (see BridgeController.cpp).
  // noinline for the same reason as above: RunLayoutStep's frame stays small.
  __attribute__((noinline)) bool LayoutWakeReady();
  // Give the wake lock back, if the write holds it.
  void ReleaseLayoutWake();

  // A hand-off that could not be armed (OBS-1).
  uint32_t layoutHandOffRefused_ {0};
  void RefuseLayoutHandOff(int version);
  // A transfer refused at its first chunk because the heap phase was held
  // (N6): one WATCHFACE_ACK, ok=false, reason "pending".
  void OnLayoutTransferRefused(uint16_t msgId);
  // The stall event ran (NimBLE host task): release a transfer that has run
  // past the reassembly timeout without completing.
  void OnLayoutTransferStalled();

  // Sensor data collector
  SensorCollector sensorCollector_;

  // REQ-011 — bounded static mailbox for inbound BLE data.
  //
  // NimBLE callbacks must not block: heavy CBOR decode work in the host-task
  // callback stalls the BLE stack and causes connection drops during dashboard
  // pushes. The producer (NimBLE host task) drops the frame into a fixed-size
  // ring; the consumer (BleProcessingTask) drains and dispatches.
  //
  // Sizing budget (plan REQ-011): 8 slots × 264 B = 2,112 B in .bss. A slot is
  // a 256-byte payload buffer + 2-byte length + 6 bytes padding so the slot
  // size hits 264 B on the natural alignment. Static asserts enforce.
  //
  // Concurrency: single-producer (NimBLE host task) / single-consumer
  // (BleProcessingTask). head_ written only by producer; tail_ written only
  // by consumer; release/acquire pairing means the consumer sees a complete
  // slot once tail catches head.
  //
  // Back-pressure: on a full ring the producer increments bleQueueOverflows_
  // and DROPS the frame; the producer never blocks the NimBLE callback.
  static constexpr size_t kBleQueueDepth = 8;
  static constexpr size_t kBleQueueItemSize = 256;
  struct BleQueueSlot {
    uint8_t data[kBleQueueItemSize];
    uint16_t len;
    uint8_t pad[6];
  };
  static_assert(sizeof(BleQueueSlot) == 264, "REQ-011 budget: 264 B per slot");
  static_assert(sizeof(BleQueueSlot) * kBleQueueDepth == 2112,
                "REQ-011 budget: 2112 B mailbox total");

  // The 2112 B ring and its indices exist only when the mailbox is compiled
  // in. With HA_BLE_RX_MAILBOX=0 the producer is not built (see
  // OnBleDataReceived), so declaring them unconditionally cost 2112 B of
  // permanently-resident .bss for code that could never run.
#if HA_BLE_RX_MAILBOX
  BleQueueSlot bleQueueSlots_[kBleQueueDepth];
  std::atomic<uint8_t> bleQueueHead_{0};   // producer-only writer
  std::atomic<uint8_t> bleQueueTail_{0};   // consumer-only writer
#endif // HA_BLE_RX_MAILBOX
  // Always present: reported in HELLO regardless of mailbox configuration.
  std::atomic<uint32_t> bleQueueOverflows_{0};

  // BleProcessingTask — gated on HA_BLE_RX_MAILBOX (disabled while boot loop
  // is being bisected; 2.4.10 used direct dispatch on the NimBLE host task).
#if HA_BLE_RX_MAILBOX
  static constexpr uint16_t kBleTaskStackWords = 1024;
  TaskHandle_t bleProcessingTask_{nullptr};
  // REQ-011 wake primitive. InfiniTime's FreeRTOSConfig has
  // configUSE_TASK_NOTIFICATIONS=0, so xTaskNotifyGive/ulTaskNotifyTake are
  // not linkable. Binary semaphore is the standard equivalent: a give in the
  // producer wakes the consumer's pending take; coalescing across multiple
  // gives is fine because the consumer drains the whole SPSC ring per wake.
  SemaphoreHandle_t bleWakeSem_{nullptr};

  static void BleProcessingTaskEntry(void* param);
  void BleProcessingTaskLoop();
#else
  // HA_BLE_RX_MAILBOX=0: direct dispatch. bleProcessingTask_ null-check in
  // OnBleDataReceived triggers the inline path without any task overhead.
  TaskHandle_t bleProcessingTask_{nullptr};
#endif // HA_BLE_RX_MAILBOX

  // InfinAssistant sensor reporting state
  bool infinassistantEnabled_ = false;
  // Period of the task that drives OnTick. MUST match the lv_task period in
  // HomeAssistant::Refresh (HomeAssistant.cpp) — every interval below is
  // derived from it, so changing the task period only requires changing this.
  static constexpr uint32_t kTickPeriodMs = 1000;
  static constexpr uint32_t kTicksPerSecond = 1000u / kTickPeriodMs;
  // A tick period slower than 1 s would make kTicksPerSecond zero and collapse
  // every derived interval to "every tick". Fail the build instead.
  static_assert(kTickPeriodMs > 0 && 1000u % kTickPeriodMs == 0 && kTicksPerSecond > 0,
                "kTickPeriodMs must divide 1000 and yield a non-zero tick rate");

  // MTU is re-polled periodically because the negotiated value can change after
  // connect. 10 s is well inside the window before the first large write.
  static constexpr uint32_t kMtuPollIntervalTicks = 10u * kTicksPerSecond;

  // Wall-clock milliseconds since boot, modulo 2^32. FreeRTOS ticks advance
  // whether or not the display is awake, unlike the OnTick invocation count.
  // The same clock as the frame codec's (CodecClockMs in the .cpp). It used to be
  // xTaskGetTickCount() * portTICK_PERIOD_MS, and that macro is 1000 / 1024 = 0
  // at configTICK_RATE_HZ 1024: this read 0 forever, so the sensor report never
  // came due and find-watch never repeated or expired. Compare values by
  // unsigned difference only.
  static uint32_t NowMs();

  // Time-based intervals are measured in wall-clock milliseconds via
  // xTaskGetTickCount(), NOT by counting OnTick invocations.
  //
  // OnTick is driven by the HA watch face's lv_task, which only runs while
  // that face is on-screen and the display is awake. Counting invocations
  // therefore measured face-awake time: a "60 s" sensor interval elapsed only
  // after 60 s of cumulative screen-on time, which on a sleeping watch is
  // hours of wall clock. FreeRTOS ticks advance regardless, so the interval is
  // now real seconds and the only remaining coupling is granularity — a report
  // fires at the first OnTick after the interval expires.
  uint32_t sensorReportIntervalMs_ = 60u * 1000u;  // 60 s default
  uint32_t lastSensorReportMs_ = 0;
  uint32_t currentTick_ = 0;
  bool bleConnected_ = false;

  // MTU (Maximum Transmission Unit) renegotiation tracking
  uint16_t lastKnownMtu_ {0};

  // A WATCHFACE_ACK the BLE stack refused, kept for retry from a context other
  // than the GATT write callback it originally failed in. See SendWatchFaceAck.
  struct PendingWatchFaceAck {
    bool        pending {false};
    bool        ok {false};
    int         version {0};
    const char* reason {nullptr};
    uint8_t     attempts {0};
  };
  PendingWatchFaceAck pendingWfAck_ {};
  static constexpr uint8_t kMaxWatchFaceAckAttempts = 8;

  // REQ-012 — count of single-frame sends rejected because the framed payload
  // exceeded the negotiated ATT MTU. Exposed in HELLO so the phone can detect
  // an under-provisioned MTU and either renegotiate or surface a UX warning.
  std::atomic<uint32_t> oversizeNotifyDrops_ {0};

  // Update FrameCodec MTU when BLE renegotiation occurs
  void SetMtu(uint16_t mtu);

  // Haptic feedback, played from the bridge's own context rather than a screen's tick.
  //
  // Patterns are requested from the display task (send failure, calendar, find-watch) and the
  // NimBLE host task (HAPTIC_FEEDBACK, ACTION_RESULT) with RequestHaptic, and played by a
  // one-shot timer ("hhp") on the FreeRTOS timer daemon, so they play on any screen and with the
  // display off. The pattern, its phase and the dedupe record are guarded by a critical section
  // (a few loads and stores). The MotorController needs no guard: RunForDuration only posts
  // timer commands.
  enum class HapticPattern : uint8_t {
    None = 0,
    Short,
    Double,
    RecordingStart,
    Long,
    FailureDouble  // REQ-019 — non-blocking failure haptic, replaces inline vTaskDelay
  };
  HapticPattern pendingHaptic_ {HapticPattern::None};
  uint8_t hapticPhase_ {0};        // 0 = first pulse next, 1 = second
  TimerHandle_t hapticTimer_ {nullptr};
  // The gap between the two pulses of a double pattern (start to start).
  static constexpr uint32_t kHapticGapMs = 125;

  // A session's final result and the phone's HAPTIC_FEEDBACK for it both ask for the same
  // buzz. Whichever comes second, within this window, is dropped.
  static constexpr uint32_t kHapticDedupMs = 1500;
  HapticPattern lastHaptic_ {HapticPattern::None};
  uint32_t lastHapticMs_ {0};
  bool lastHapticFromVoice_ {false};

  // Ask for a pattern. `onlyIfIdle`: drop it when another pattern is waiting. `fromVoiceResult`:
  // it comes from a voice session's final result (see the dedupe above).
  void RequestHaptic(HapticPattern pattern, bool onlyIfIdle = false, bool fromVoiceResult = false);
  // One step of the player (timer daemon): a pulse, and the re-arm for the second one.
  void StepHaptic();
  bool ArmHapticTimer(TickType_t ticks);
  void DropHaptic();
  static void HapticTimerCb(TimerHandle_t timer);

  // The phone has voice assist on: the voiceAssist key of its last CONFIG_PUSH (absent = off).
  // Cleared when the link drops (OnBleDisconnected). Until it is set the double-click is not
  // ours: a GAP connection alone does not say the app is there, or that it can do voice.
  std::atomic<bool> voiceEnabled_ {false};

  // Voice session (0xFFFE). Active from the trigger, or from a non-final ACTION_RESULT, until a
  // final one ("done", "cancelled" or ok=false) or a timeout, so a lost message cannot strand
  // it. From the trigger to the first result the timeout is the short kVoiceStartTimeoutMs;
  // each result then restarts the long kVoiceSessionTimeoutMs. Written by the display task and
  // the NimBLE host task, so state and stamp are ONE atomic word and a timeout clears it with
  // a compare-exchange (it cannot wipe a result that just arrived):
  //   bit 0 active, bit 1 a result has been seen, bits 2-31 NowMs() at the last change (30 bits).
  static constexpr uint32_t kVoiceStartTimeoutMs = 10u * 1000u;
  static constexpr uint32_t kVoiceSessionTimeoutMs = 60u * 1000u;
  std::atomic<uint32_t> voiceState_ {0};
  bool VoiceSessionActive();
  void MarkVoiceSession(bool active, bool resultSeen);

  // Encode and send one ACTION_INVOKE. Display task only: it uses the static buffer of
  // SendFromUi and its own static one. entityId may be null.
  bool SendActionInvoke(uint16_t actionId, const char* gesture, const char* entityId);

  // REQ-068 — pending action tracking (set in OnActionInvoked, consumed in HandleActionResult).
  // 0xFFFF means no action pending. Only one ACTION_INVOKE is in flight at a time
  // (the watch serialises tile taps via the confirm/haptic gate).
  uint16_t pendingActionId_        {0xFFFF};
  uint8_t  pendingActionTileIndex_ {0xFF};   // 0xFF = unknown tile

  // REQ-070 — drain calendar-event vibration request from HomeAssistant screen.
  void ProcessCalendarVibration();

  // REQ-072 — FIND_WATCH alert state.
  // NowMs() deadline until which the watch should vibrate periodically.
  // Zero means no find-watch alert is active.
  static constexpr uint32_t kFindWatchDurationMs = 30u * 1000u;
  // One buzz per OnTick. The tick is an lv_task with a 1000 ms period on LVGL's
  // clock, which counts FreeRTOS ticks (1024 Hz), so it runs every ~977 ms of
  // NowMs time and a 1000 ms threshold would skip every other tick.
  static constexpr uint32_t kFindWatchVibrateIntervalMs = 900u;
  uint32_t findWatchUntilMs_ = 0;
  uint32_t lastFindWatchVibrateMs_ = 0;
  void ProcessFindWatch();

  // Handlers — accept raw pointers to avoid heap-allocating std::vector per message.
  // WHY: Every BLE message previously created 3-4 vectors (callback, chunk, map node,
  // payload). Over 20+ minutes idle (~600+ messages), this fragmented the heap until
  // operator new failed on screen transition → crash.
  void OnBleDataReceived(const uint8_t* data, size_t len);
  void OnFrameReceived(const Frame& frame);

  // Message handlers (raw pointer payload — no vector allocation)
  void HandleHello(const uint8_t* payload, size_t len, uint16_t msgId);
  void HandleHelloAck(const uint8_t* payload, size_t len);
  void HandleConfigPush(const uint8_t* payload, size_t len, uint16_t msgId);
  void HandleStateUpdate(const uint8_t* payload, size_t len);
  void HandleNotify(const uint8_t* payload, size_t len, uint16_t msgId);
  // 2.4.101 — NOTIFY_REMOVE (phone → watch): silent, no ACK.
  void HandleNotifyRemove(const uint8_t* payload, size_t len);
  void HandlePing(uint16_t msgId);
  void HandleHapticFeedback(const uint8_t* payload, size_t len);
  void HandleWatchFaceLayout(const uint8_t* payload, size_t len, uint16_t msgId);
  void HandleTimeSync(const uint8_t* payload, size_t len);
  void HandleLogbookUpdate(const uint8_t* payload, size_t len);
  void HandleCalendarUpdate(const uint8_t* payload, size_t len);
  void HandleHomeControlUpdate(const uint8_t* payload, size_t len);
  void HandleDashboardState(const uint8_t* payload, size_t len);
  // REQ-059 — crash-dump handlers (phone → watch). With HA_CRASH_RING they
  // serve the 4-slot ring; without it, a ring of one: the reset reason and
  // previousCrashContext_.
  void HandleCrashDumpRequest(const uint8_t* payload, size_t len, uint16_t msgId);
  void HandleCrashDumpAck(const uint8_t* payload, size_t len);
  // REQ-072 — FIND_WATCH handler (phone → watch).
  void HandleFindWatch(const uint8_t* payload, size_t len, uint16_t msgId);
  // REQ-068 — ACTION_RESULT handler (phone → watch: reply to ACTION_INVOKE).
  void HandleActionResult(const uint8_t* payload, size_t len);
  // Send a single-frame message with task-isolated static buffers.
  // BLE-task callers (HandleHello, HandleConfigPush, HandlePing, etc.) use SendFromBle.
  // Display-task callers (OnActionInvoked, SendSensorReport) use SendFromUi.
  // Two separate static frame[256] buffers prevent cross-task data corruption.
  bool SendSingleFrameImpl(MessageType type, const uint8_t* payload, size_t payloadLen, uint8_t* frame, size_t frameSize);
  bool SendFromBle(MessageType type, const uint8_t* payload, size_t payloadLen);
  bool SendFromUi(MessageType type, const uint8_t* payload, size_t payloadLen);

  void SendConfigAck(bool ok, int configVersion);
  void SendNotifyAck(uint16_t msgId, const char* tag);
  // 2.4.101 — NOTIFY_DISMISS {"nid": n}. Display task only: the user dismissed phone notification n.
  void SendNotifyDismiss(uint16_t nid);
  void SendWatchFaceAck(bool ok, int version, const char* reason = nullptr);
  /** Encode + send one WATCHFACE_ACK; false when the notify was refused. */
  bool EmitWatchFaceAck(bool ok, int version, const char* reason);
  /** Retry a queued WATCHFACE_ACK. Safe to call from any context. */
  void FlushPendingWatchFaceAck();
};

} // namespace HaBridge
} // namespace Applications
} // namespace Pinetime
