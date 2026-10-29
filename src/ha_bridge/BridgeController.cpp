#include "BridgeController.h"
#include "DashboardLimits.h"
#include "Version.h"
#include "WakeDiag.h"
#include "components/datetime/DateTimeController.h"
#include "drivers/Watchdog.h"
#include <cbor.h>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <new>
#include <FreeRTOS.h>
#include <portable.h>    // vPortFree: a handed-off reassembly buffer that is not adopted
#include <task.h>
#include <timers.h>
#define min  // nimble's min/max macros conflict with libstdc++ (see NimbleController.cpp)
#define max
#include <nimble/nimble_npl.h>   // ble_npl_event, ble_npl_eventq_put: the stalled-transfer watch
#include <nimble/nimble_port.h>  // nimble_port_get_dflt_eventq: the host task's queue
#undef max
#undef min
#include <lvgl/lvgl.h>   // lv_mem_monitor_t, lv_mem_monitor() — LVGL 7.x (REQ-056 diag)

// Layout CBOR decoding lives in ha_bridge/LayoutDecoder.{h,cpp} so the LIVE
// decoder can be fuzzed on the host — see that header. Must be included at
// file scope: the header opens namespace Pinetime::Applications::HaBridge, so
// including it from inside that namespace nests a second copy and breaks every
// name lookup after it.
#include "ha_bridge/LayoutDecoder.h"
#include "ha_bridge/LayoutDocumentDecoder.h"
#include "ha_bridge/LayoutCommit.h"
#include "ha_bridge/LayoutStoreFlash.h"
#include "ha_bridge/BootConfirm.h"
#include "ha_bridge/LinkReport.h"
#include "ha_bridge/NotifyDecoder.h"
#include "ha_bridge/SensorReport.h"
// SCB->CFSR / HFSR / MMFAR / BFAR on nRF52832 (Cortex-M4). Unconditional
// because the fault handlers that read them are unconditional now.
#include <nrf.h>

// Global .noinit crash context — survives soft resets and watchdog resets.
// Defined here (single translation unit) to avoid duplicate symbols.
Pinetime::Applications::HaBridge::CrashContext
  Pinetime::Applications::HaBridge::g_crashContext __attribute__((section(".noinit")));

#if HA_WATCHDOG_SUBSYSTEM_AWARE
// REQ-060 — per-subsystem heartbeat flags in .noinit.
// Each entry is written by its owning task/ISR (byte-atomic on Cortex-M4).
// Cleared by CheckAndKickWatchdog (called from OnTick, task context).
// Survives soft reset — CheckAndKickWatchdog re-initialises them after each kick.
volatile uint8_t Pinetime::Applications::HaBridge::g_watchdogHeartbeat[
  Pinetime::Applications::HaBridge::kWatchdogSubsystemCount]
  __attribute__((section(".noinit")));
#endif // HA_WATCHDOG_SUBSYSTEM_AWARE

// REQ-059 — 4-slot crash-dump ring in .noinit RAM.
// Gated on HA_CRASH_CONTEXT_EXTENDED: disabled while the 2.4.11+ boot loop
// is being bisected. The ring's .noinit block is a boot-path delta vs 2.4.10.
#if HA_CRASH_CONTEXT_EXTENDED && HA_CRASH_RING
Pinetime::Applications::HaBridge::CrashRing
  Pinetime::Applications::HaBridge::g_crashRing __attribute__((section(".noinit")));
#endif // HA_CRASH_CONTEXT_EXTENDED && HA_CRASH_RING

// REQ-058: ARM Cortex-M fault handlers wired under HA_CRASH_CONTEXT_EXTENDED.
//
// Each handler captures the stacked exception frame (SP points to r0-r3, r12, lr, pc, xpsr)
// and the ARM fault status registers directly into g_crashContext, then loops forever
// so the watchdog triggers a reset. No FreeRTOS, heap, or LVGL calls are made —
// the only writes are to .noinit RAM fields via plain 32-bit stores.
//
// Cortex-M4 exception frame layout (from Arm ARMv7-M Architecture Reference Manual,
// Table B1-8): SP+0=r0, SP+4=r1, SP+8=r2, SP+12=r3, SP+16=r12, SP+20=lr, SP+24=pc,
// SP+28=xpsr.
//
// These symbols must be defined with C linkage so the ARM vector table (defined in
// InfiniTime's startup_nrf52832.s) can resolve them without name mangling.
// ARM fault handlers: RE-GATED in 2.4.54, and the reasoning above them was
// wrong.
//
// "They only execute on a fault, so they cannot affect a healthy boot" is
// sound in principle and false in practice here. Across 13 builds, the size of
// HardFault_Handler predicts booting perfectly:
//
//   0x02 (weak `B .`)  2.4.40, .44-.50   all boot
//   0x12 (these)       2.4.41, .42, .51, .52, .53   all boot-loop
//
// with no exception. It also rules out the memory explanations that were
// suspected for months: 2.4.41 loops with 32688 B of heap while the working
// 2.4.44-2.4.48 have 32656 B — less heap boots, more heap loops.
//
// Why merely DEFINING them breaks boot is not yet established. Installing them
// overrides weak symbols in the nRF startup/SDK vector table, so the candidates
// are a duplicate or displaced vector entry, or the SoftDevice's expectations
// about which exceptions the application owns — not anything these bodies do.
//
// Gated off until that is understood. This is the ONLY difference between
// 2.4.53, which loops, and 2.4.54, which keeps the CBOR overflow fix.
#if HA_CRASH_CONTEXT_EXTENDED
extern "C" {

// CaptureFaultContext — non-static C-linkage function called via branch from
// each naked fault handler.  Receives the stacked exception frame pointer in r0
// (first argument register per ARM AAPCS).  Writes fault state directly into
// .noinit g_crashContext with no FreeRTOS, heap, or LVGL calls, then spins
// forever so the watchdog triggers a clean reset.
//
// Cortex-M4 exception frame layout (ARMv7-M ARM Table B1-8):
//   frame[0]=r0  frame[1]=r1  frame[2]=r2  frame[3]=r3
//   frame[4]=r12 frame[5]=lr  frame[6]=pc  frame[7]=xpsr
__attribute__((noreturn))
void CaptureFaultContext(const uint32_t* frame) {
  using namespace Pinetime::Applications::HaBridge;
  g_crashContext.magic      = kCrashContextMagic;
  g_crashContext.pc         = frame[6];   // stacked PC (instruction that faulted)
  g_crashContext.lr         = frame[5];   // stacked LR (return address to caller)
  g_crashContext.xpsr       = frame[7];   // stacked xPSR (processor state)
  g_crashContext.cfsr       = SCB->CFSR;  // Configurable Fault Status Register
  g_crashContext.hfsr       = SCB->HFSR;  // HardFault Status Register
  g_crashContext.mmfar      = SCB->MMFAR; // MemManage Fault Address Register
  g_crashContext.bfar       = SCB->BFAR;  // BusFault Address Register
  // taskName / breadcrumb remain from last SetBreadcrumb() call — intentional.

#if HA_CRASH_RING
  // REQ-059 — push a copy into the crash-dump ring (ISR-safe: plain stores only).
  // The ring magic may be uninitialised on the very first fault before Init();
  // check the magic and re-init inline so we never write into garbage RAM.
  if (g_crashRing.hdr.magic != kCrashRingMagic) {
    g_crashRing.hdr.magic = kCrashRingMagic;
    g_crashRing.hdr.head  = 0;
    g_crashRing.hdr.tail  = 0;
    g_crashRing.hdr.count = 0;
  }
  // Write slot at head.  If ring is full (count == kCrashRingSlots) the oldest
  // unread entry is overwritten (tail advances to discard it).
  uint8_t h = g_crashRing.hdr.head;
  g_crashRing.slots[h] = g_crashContext;
  h = static_cast<uint8_t>((h + 1u) % kCrashRingSlots);
  g_crashRing.hdr.head = h;
  if (g_crashRing.hdr.count < kCrashRingSlots) {
    g_crashRing.hdr.count++;
  } else {
    // Overwrite oldest: advance tail to discard it.
    g_crashRing.hdr.tail = static_cast<uint8_t>(
      (g_crashRing.hdr.tail + 1u) % kCrashRingSlots);
  }
#endif // HA_CRASH_RING

  // Spin forever: watchdog fires and performs a clean reboot, preserving .noinit.
  for (;;) { __asm volatile("nop"); }
}

// HardFault_Handler — overrides the weak symbol in startup_nrf52832.s.
// Naked: compiler emits no prologue/epilogue; we read SP before it is modified.
// EXC_RETURN bit 2: 0 = MSP (exception occurred in handler mode), 1 = PSP (thread mode).
__attribute__((naked))
void HardFault_Handler(void) {
  __asm volatile(
    "tst   lr, #4        \n"   // test EXC_RETURN bit 2
    "ite   eq            \n"
    "mrseq r0, msp       \n"   // bit 2 = 0 → frame on MSP
    "mrsne r0, psp       \n"   // bit 2 = 1 → frame on PSP
    "b     CaptureFaultContext \n"
    ::: "r0"
  );
}

// MemManage_Handler — MPU violation (reads from protected region, etc.).
__attribute__((naked))
void MemManage_Handler(void) {
  __asm volatile(
    "tst   lr, #4        \n"
    "ite   eq            \n"
    "mrseq r0, msp       \n"
    "mrsne r0, psp       \n"
    "b     CaptureFaultContext \n"
    ::: "r0"
  );
}

// BusFault_Handler — bus error (precise or imprecise).
__attribute__((naked))
void BusFault_Handler(void) {
  __asm volatile(
    "tst   lr, #4        \n"
    "ite   eq            \n"
    "mrseq r0, msp       \n"
    "mrsne r0, psp       \n"
    "b     CaptureFaultContext \n"
    ::: "r0"
  );
}

// UsageFault_Handler — undefined instruction, unaligned access, divide-by-zero.
__attribute__((naked))
void UsageFault_Handler(void) {
  __asm volatile(
    "tst   lr, #4        \n"
    "ite   eq            \n"
    "mrseq r0, msp       \n"
    "mrsne r0, psp       \n"
    "b     CaptureFaultContext \n"
    ::: "r0"
  );
}

} // extern "C"
#elif HA_FAULT_TRACE || HA_BOOT_TRACE
extern "C" {

#if HA_FAULT_TRACE
// Diagnostic (2.4.69): record the fault in the .noinit crash context, which survives the
// watchdog reset and MCUboot's rollback, so the image rolled back to reports it. frame[7]
// is the stacked xPSR, whose IPSR field is non-zero when the fault hit an interrupt
// handler; otherwise the current task's name picks the code. Plain reads and stores.
__attribute__((noreturn, used)) void HaTraceFault(const uint32_t* frame) {
  uint8_t code = 7;
  if ((frame[7] & 0x1FFu) != 0u) {
    code = 8;
  } else if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
    const char* name = pcTaskGetName(nullptr);
    if (name != nullptr) {
      if (std::strcmp(name, "displayapp") == 0) code = 1;
      else if (std::strcmp(name, "MAIN") == 0) code = 2;
      else if (std::strcmp(name, "ble") == 0) code = 3;
      else if (std::strcmp(name, "ll") == 0) code = 4;
      else if (std::strcmp(name, "Tmr Svc") == 0) code = 5;
      else if (std::strcmp(name, "Heartrate") == 0) code = 6;
    }
  }
  using Pinetime::Applications::HaBridge::g_crashContext;
  using Pinetime::Applications::HaBridge::kCrashContextMagic;
  const uint32_t cfsr = SCB->CFSR;
  const uint8_t step = static_cast<uint8_t>(g_crashContext.breadcrumb >> 4);
  g_crashContext.magic = kCrashContextMagic;
  g_crashContext.breadcrumb = static_cast<uint8_t>(0xF0u | code);
  g_crashContext.lastMsgType = static_cast<uint8_t>((step << 4) | ((cfsr >> 16) & 0x0Fu));
  g_crashContext.lastMsgId = static_cast<uint16_t>(cfsr & 0xFFFFu);
  g_crashContext.freeHeapAtCrash = frame[6];     // stacked PC
  g_crashContext.uptimeTicksAtCrash = frame[5];  // stacked LR
  for (;;) {
    // No watchdog reload: it resets the watch, and MCUboot rolls an unconfirmed image back.
  }
}

__attribute__((naked))
void HardFault_Handler(void) {
  __asm volatile(
    "tst   lr, #4        \n"
    "ite   eq            \n"
    "mrseq r0, msp       \n"
    "mrsne r0, psp       \n"
    "b     HaTraceFault  \n"
    ::: "r0"
  );
}
#endif // HA_FAULT_TRACE

#if HA_BOOT_TRACE
// Diagnostic (2.4.72): SystemTask start-up stage, in lastMsgType, with the tick (low 16 bits)
// in lastMsgId. The weak no-ops in BootConfirm.cpp stand in for the executables without this
// file, here and for HaBootMarkStep.
void HaBootMarkInitStage(uint8_t stage) {
  using Pinetime::Applications::HaBridge::g_crashContext;
  using Pinetime::Applications::HaBridge::kCrashContextMagic;
  g_crashContext.magic = kCrashContextMagic;
  g_crashContext.lastMsgType = stage;
  g_crashContext.lastMsgId = static_cast<uint16_t>(xTaskGetTickCount());
}

// Diagnostic (2.4.69): BootConfirm::MarkStep's recorder. The boot step goes in the high nibble
// of the breadcrumb (SetBreadcrumb keeps it), with the time and the free heap. It only ever
// rises within a boot (BridgeController::Init clears it), so the record holds the furthest step
// reached: the face is built again on later screen changes and repeats 1 and 2.
void HaBootMarkStep(uint8_t step) {
  using Pinetime::Applications::HaBridge::g_crashContext;
  using Pinetime::Applications::HaBridge::kCrashContextMagic;
  const uint8_t current = static_cast<uint8_t>(g_crashContext.breadcrumb >> 4);
  if (step <= current) {
    return;
  }
  g_crashContext.magic = kCrashContextMagic;
  g_crashContext.breadcrumb = static_cast<uint8_t>((step << 4) | (g_crashContext.breadcrumb & 0x0Fu));
  g_crashContext.freeHeapAtCrash = xPortGetFreeHeapSize();
  g_crashContext.uptimeTicksAtCrash = xTaskGetTickCount();
}
#endif // HA_BOOT_TRACE

} // extern "C"
#endif // HA_CRASH_CONTEXT_EXTENDED / HA_FAULT_TRACE || HA_BOOT_TRACE

// CRC-32 (ISO 3309 / ITU-T V.42) using a 16-entry nibble table.
// 64 bytes in .rodata (flash), zero RAM cost.  ~3× slower than a 1KB table
// but adequate for one layout push at a time (~few KB payload).
static uint32_t Crc32Nibble(const uint8_t* data, size_t len) {
  static constexpr uint32_t kTable[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
    0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
    0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
  };
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc = (crc >> 4) ^ kTable[(crc ^ data[i]) & 0x0F];
    crc = (crc >> 4) ^ kTable[(crc ^ (data[i] >> 4)) & 0x0F];
  }
  return crc ^ 0xFFFFFFFFu;
}

// FreeRTOS malloc/stack overflow counters (defined in InfiniTime main.cpp).
// Extern here so we can read them for diagnostics.
extern int mallocFailedCount;
extern int stackOverflowCount;

namespace Pinetime {
namespace Applications {
namespace HaBridge {

#if HA_CRASH_CONTEXT_EXTENDED
// UpdateCrashContextLvgl — snapshots LVGL pool stats into g_crashContext.
// Called from BridgeController task context (not ISR-safe) on each SetBreadcrumb
// call that originates from inside the BLE processing task, where LVGL headers
// are available. Kept as a non-inline function to confine the lv_mem_monitor
// call to this translation unit (which includes lvgl/lvgl.h).
void UpdateCrashContextLvgl() {
#if HA_CRASH_RUNTIME_PROBES
  lv_mem_monitor_t mon = {};
  lv_mem_monitor(&mon);
  g_crashContext.lvFreeSize    = static_cast<uint32_t>(mon.free_size);
  g_crashContext.lvFreeBiggest = static_cast<uint32_t>(mon.free_biggest_size);
#endif // HA_CRASH_RUNTIME_PROBES
  // With probes off these stay zero. Note lv_mem_monitor() is useless under
  // LV_MEM_CUSTOM=1 anyway (LVGL allocates via pvPortMalloc and never fills the
  // monitor), which is why CheckSafeFaceCondition uses xPortGetFreeHeapSize().
}
#endif // HA_CRASH_CONTEXT_EXTENDED



// REQ-014 — INVARIANT: BridgeController is singleton-for-lifetime. The
// HomeAssistantService callbacks installed below capture `this`; the dtor
// does NOT unregister them, and the BLE/UI/sensor paths assume they can
// fire-and-forget through the std::function setters without a lifetime
// check. SystemTask constructs the only instance and never replaces it.
// instanceCount_ asserts the invariant: if anyone ever constructs a second
// instance, ConfigASSERT fires immediately rather than letting dangling
// pointers cause non-deterministic crashes later. If multi-instance ever
// becomes a real requirement, BridgeController::~BridgeController() must
// clear the callbacks via bleService_.SetOnDataReceived({}) etc and the
// setters need to gate against concurrent fire.
namespace {
uint8_t g_bridgeControllerInstanceCount_ = 0;
// The step timer's callback carries no state of its own (see LayoutPipeline::StepCb).
Pinetime::Applications::HaBridge::BridgeController* g_layoutStepOwner = nullptr;

// Milliseconds since boot for the frame codec's reassembly timeout. Not
// xTaskGetTickCount() * portTICK_PERIOD_MS: at configTICK_RATE_HZ 1024 that
// macro is 1000 / 1024 = 0 in integer arithmetic, so the product is always 0 and
// the timeout could never expire. The 64-bit product cannot overflow, and the
// divisor is a power of two, so this is a multiply and a shift.
//
// The result is the millisecond count MODULO 2^32. The 32-bit tick counter wraps
// every 2^32 ticks (about 48.5 days at 1024 Hz), and scaling it straight to
// milliseconds would drop from about 4.19e9 to 0 there, not wrap at 2^32. So the
// wraps are counted and the 64-bit tick count is scaled: the low 32 bits of that
// are continuous through the tick wrap. The counting needs a call at least once
// per tick period; the codec calls it on every BLE write. Every comparison it
// feeds is an unsigned difference (FrameCodec.cpp), which is correct across the
// 2^32 wrap of this value itself. Called from tasks only (the host task, the
// timer daemon, and the display task through NowMs): the critical section makes
// the wrap count and the tick one consistent pair.
static_assert(sizeof(TickType_t) == 4, "the tick wrap counter assumes a 32-bit tick");
uint32_t CodecClockMs() {
  static uint32_t lastTick = 0;
  static uint32_t tickWraps = 0;
  taskENTER_CRITICAL();
  const uint32_t tick = static_cast<uint32_t>(xTaskGetTickCount());
  if (tick < lastTick) {
    ++tickWraps;
  }
  lastTick = tick;
  const uint32_t wraps = tickWraps;
  taskEXIT_CRITICAL();
  const uint64_t ticks = (static_cast<uint64_t>(wraps) << 32) | tick;
  return static_cast<uint32_t>((ticks * 1000u) / configTICK_RATE_HZ);
}

// The stall watch fires this long after a layout transfer begins: the
// reassembly timeout plus a slack that keeps it from firing at the very
// millisecond the codec's `>` test still calls the transfer live.
//
// The watch is the layout timer itself (LayoutPipeline::timer), which has two
// roles that never overlap. While a transfer reassembles no write is pending, so
// the timer is idle and TryBegin arms it for this period (stall watchdog). At the
// hand-off the handler re-arms it with kStepPeriodTicks, and from then on it
// paces the write steps. RunLayoutStep tells the two apart by the timer's period.
constexpr uint32_t kStallSlackMs = 100;
constexpr uint32_t kStallWatchMs = Pinetime::Applications::HaBridge::DashboardLimits::REASSEMBLY_TIMEOUT_MS + kStallSlackMs;
constexpr TickType_t kStepPeriodTicks = 1;
// How long a layout write waits for the system to reach Running after its wake
// request before it is dropped (LayoutWakeReady).
constexpr TickType_t kWakeWaitTicks = pdMS_TO_TICKS(2000);

// The disconnect abort and the stall handler call the codec from the NimBLE host
// task, which is the task that runs Ingest only while the RX mailbox is off. With
// the mailbox on, Ingest runs on the BleProc task and both would race it.
static_assert(!HA_BLE_RX_MAILBOX, "the codec abort paths assume Ingest runs on the NimBLE host task");
}

uint32_t BridgeController::NowMs() {
  return CodecClockMs();
}

// The layout write path. One instance, placed in .bss by the constructor. It is
// also the reassembler's transfer gate: the first chunk of a WATCHFACE_LAYOUT
// transfer takes the heap phase (LayoutWriter::TryBeginTransfer), every path that
// drops the transfer gives it back (End), and a refusal is reported to the
// controller so the phone gets an ACK instead of a 15 s timeout.
struct BridgeController::LayoutPipeline final : public TransferGate {
  LayoutPipeline(BridgeController& controller, HeapPhaseCell& phase) : writer(store, phase), owner(controller) {}

  // The first chunk of a transfer takes the heap phase and starts the stall
  // watch. Both run on the NimBLE host task (inside Ingest). The watch is never
  // stopped: when the transfer ends, by any path, its later expiry finds nothing
  // in flight and does nothing, and a newer transfer restarts it.
  //
  // NFR-4: this runs inside the GATT write callback and must not block. The watch
  // is armed with ONE zero-tick xTimerChangePeriod on the layout timer: it
  // restarts a running timer from now and starts a dormant one. The timer is idle
  // here (no write can be pending while this transfer holds the heap phase). On
  // expiry the daemon posts stallEvent to the host task's queue (RunLayoutStep),
  // and StallCb runs there. If the command cannot be queued the watch is skipped
  // for this transfer (counted); the disconnect clear and the next-frame timeout
  // still bound the stall.
  bool TryBegin() override {
    // The gate is installed in the constructor, before advertising starts; the
    // layout timer and the stall event are created in Init(). A push in that
    // window is refused here (the phone gets the "pending" notice and retries).
    if (!ready.load(std::memory_order_acquire)) {
      ++notReadyRefused;
      return false;
    }
    if (!writer.TryBeginTransfer()) {
      return false;
    }
    if (timer != nullptr &&
        xTimerChangePeriod(timer, static_cast<TickType_t>(ble_npl_time_ms_to_ticks32(kStallWatchMs)), 0) != pdPASS) {
      ++stallArmFailed;
    }
    return true;
  }
  void End() override { writer.AbortTransfer(); }
  void OnRefused(uint16_t msgId) override { owner.OnLayoutTransferRefused(msgId); }
  // The rest of a refused transfer's chunks: they never complete a frame, so
  // this is where a refusal notice that could not be sent is retried.
  void OnChunkDropped() override { owner.FlushPendingWatchFaceAck(); }

  // The step timer's callback, on the timer daemon: a bare tail call, no locals
  // and no call before it, so it leaves no frame on the daemon stack. The
  // controller is a singleton, so the callback reaches it through the pointer
  // the constructor stored; the timer's ID carries the same pointer.
  static void StepCb(TimerHandle_t timer) {
    (void)timer;
    g_layoutStepOwner->RunLayoutStep();
  }

  // The stall watch's handler, on the NimBLE host task (the event is on that
  // task's queue). It acts only on what the codec still holds: a transfer that
  // completed, was handed off or was cleared is not touched.
  static void StallCb(struct ble_npl_event* event) {
    (void)event;
    g_layoutStepOwner->OnLayoutTransferStalled();
  }

  // The layout timer expired in its stall role: hand the check to the host task.
  // ble_npl_eventq_put ignores an event that is already queued, and is the same
  // call the NimBLE callout's expiry made from this task. noinline: it keeps the
  // frame off RunLayoutStep, which is live across every write step.
  __attribute__((noinline)) void PostStall() { ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &stallEvent); }

  LayoutStoreFlash store;
  LayoutWriter writer;
  BridgeController& owner;
  uint32_t stallArmFailed {0};  // stall watches that could not be queued (NimBLE host task only)
  uint32_t notReadyRefused {0};  // transfers refused before Init() finished (NimBLE host task only)
  std::atomic<bool> ready {false};  // Init() created the layout timer and set up the stall event
  bool wakeHeld {false};  // the write holds a system wake lock (timer daemon only)
  TickType_t wakeAskedAt {0};  // tick of the wake request (timer daemon only)
  TimerHandle_t timer {};
  struct ble_npl_event stallEvent {};
};

BridgeController::BridgeController(
  Pinetime::Controllers::HomeAssistantService& bleService,
  Pinetime::Controllers::MotionController& motionController,
  Pinetime::Controllers::HeartRateController& heartRateController,
  Pinetime::Controllers::Battery& batteryController,
  Pinetime::Controllers::MotorController* motorController,
  Pinetime::Controllers::DateTime* dateTimeController,
  Pinetime::Drivers::Watchdog* watchdog
)
  : bleService_(bleService),
    motorController_(motorController),
    dateTimeController_(dateTimeController),
    watchdog_(watchdog),
    sensorCollector_(motionController, heartRateController, batteryController) {
  // REQ-014 singleton invariant check (see comment above the class definition).
  configASSERT(g_bridgeControllerInstanceCount_ == 0);
  ++g_bridgeControllerInstanceCount_;

  // The layout write path lives in .bss: the singleton invariant above means
  // this storage is constructed exactly once. The codec gates every
  // WATCHFACE_LAYOUT transfer (single chunk included) through it.
  alignas(LayoutPipeline) static uint8_t layoutPipelineMem[sizeof(LayoutPipeline)];
  layout_ = new (layoutPipelineMem) LayoutPipeline(*this, bleService_.HeapPhaseByte());
  g_layoutStepOwner = this;
  frameCodec_.SetTransferGate(layout_, static_cast<uint8_t>(MessageType::WATCHFACE_LAYOUT));

  // Wire BLE data received callback — pass raw pointer directly (no vector construction).
  // Previously: [this](data, len) { std::vector<uint8_t> vec(data, data+len); OnBleDataReceived(vec); }
  // Each vector construction was a heap alloc+free per BLE write, ~600+ times over 20 min idle.
  bleService_.SetOnDataReceived(
    [this](const uint8_t* data, size_t len) {
      OnBleDataReceived(data, len);
    }
  );

  // Wire action invoked callback
  bleService_.SetOnActionInvoked(
    [this](uint16_t actionId, const char* gesture) {
      OnActionInvoked(actionId, gesture);
    }
  );

  // Wire the periodic tick. Without this, ProcessTick() is a no-op and every
  // subsystem driven by OnTick is dead: sensor reporting, deferred haptics,
  // find-watch vibration, calendar-start vibration and the MTU re-poll.
  //
  // IMPORTANT — the tick source is the HA watch face's lv_task
  // (HomeAssistant::Refresh -> ProcessTick), which is created when that face is
  // shown and deleted when it is left, and which only runs while the display is
  // awake. currentTick_ therefore advances in face-awake time, not wall-clock
  // time: a "60 s" sensor interval is 60 s of cumulative on-screen time, and
  // find-watch vibration pauses when the screen sleeps. Giving BridgeController
  // its own FreeRTOS timer would decouple this; that is deliberately out of
  // scope here because it changes task-context assumptions across the file.
  bleService_.SetOnTick(
    [this]() {
      OnTick();
    }
  );

  // The user dismissed a phone notification on the watch (2.4.101): tell the phone. Runs on the
  // display task, from the notification list's dismiss hook.
  bleService_.SetOnNotifyDismissed(
    [this](uint16_t nid) {
      SendNotifyDismiss(nid);
    }
  );

}

BridgeController::~BridgeController() {
}

void BridgeController::CaptureEarly() {
  if (g_crashContext.magic == kCrashContextMagic) {
    previousCrashContext_ = g_crashContext;
    hasCrashContext_ = true;
  } else {
    // Captured, nothing valid: any non-zero value other than the magic stops Init capturing
    // this boot's own stages as "the previous boot".
    previousCrashContext_.magic = 0x0BADF00Du;
  }
}

void BridgeController::Init() {
  // BLE service already initialized by NimbleController
  // Frame codec is ready

  // The layout write's step timer: one resident one-shot timer on the FreeRTOS
  // timer daemon, created once and never deleted. A dedicated bridge task was
  // left out on purpose: the daemon already exists (a task and its stack would
  // cost about 2 KB of heap, 3b.1 redo option B6), it is independent of the face
  // and the display, and it is not the watchdog feeder. Its stack is raised from
  // 300 to 470 words for this (configTIMER_TASK_STACK_DEPTH). If the timer cannot
  // be created every layout push is refused at hand-off and the previous layout
  // stays.
  layout_->timer = xTimerCreate("hlw", kStepPeriodTicks, pdFALSE, this, &LayoutPipeline::StepCb);

  // The haptic player's timer, on the same daemon (see RequestHaptic). Created once, never
  // deleted; if it cannot be created haptics are silent.
  hapticTimer_ = xTimerCreate("hhp", 1, pdFALSE, this, &BridgeController::HapticTimerCb);

  // The stalled-transfer watch is the same timer (see kStepPeriodTicks): while a
  // transfer reassembles it is armed for the stall period, and on expiry the
  // daemon posts this one static event to the NimBLE host task's queue, so the
  // handler runs on the task that runs Ingest and needs no lock. No callout and no
  // second timer block. If the timer could not be created (handle stays null) the
  // watch is skipped and only the disconnect abort and the next-frame timeout
  // remain.
  ble_npl_event_init(&layout_->stallEvent, &LayoutPipeline::StallCb, nullptr);

  // The timer and the event exist now (a failed timer stays null, which the paths above tolerate):
  // from here the gate may admit a transfer.
  layout_->ready.store(true, std::memory_order_release);

  // REQ-011 — bring up the static mailbox + dedicated processing task.
  // The 2.4.x line previously left the queue infrastructure DISABLED because
  // v1.103.8's FreeRTOS-queue + per-item heap alloc exhausted the heap on
  // 64 KB PineTimes. The current implementation eliminates per-message heap
  // traffic: the mailbox lives in .bss (2,112 B) and the consumer task is
  // created once at boot (one-time ~4 KB heap for stack). Priority is
  // tskIDLE_PRIORITY + 1 — below the display refresh task, above idle,
  // matching the plan's "below display, above idle" constraint.
#if HA_BLE_RX_MAILBOX
  bleWakeSem_ = xSemaphoreCreateBinary();
  if (!bleWakeSem_) {
    bleProcessingTask_ = nullptr;  // direct dispatch fallback
  } else {
    BaseType_t taskOk = xTaskCreate(
      &BleProcessingTaskEntry,
      "BleProc",
      kBleTaskStackWords,
      this,
      tskIDLE_PRIORITY + 1,
      &bleProcessingTask_);
    if (taskOk != pdPASS) {
      bleProcessingTask_ = nullptr;  // OnBleDataReceived falls back to direct dispatch
    }
  }
#else
  bleProcessingTask_ = nullptr;  // direct dispatch — mailbox disabled
#endif // HA_BLE_RX_MAILBOX

  // Capture crash context from previous boot (if magic word is valid).
  // After reading, clear the magic so stale data isn't reported again.
  // previousCrashContext_ is all zero until a capture: CaptureEarly may have taken the record
  // already, before this boot's start-up stages wrote to it.
  if (previousCrashContext_.magic == 0 && g_crashContext.magic == kCrashContextMagic) {
    previousCrashContext_ = g_crashContext;
    hasCrashContext_ = true;
  }
  g_crashContext.magic = 0;
  g_crashContext.breadcrumb = static_cast<uint8_t>(Breadcrumb::IDLE);

#if HA_BOOT_FIXES
  // Start from an empty layout store when the previous boot died before its first store read was
  // drawn (the boot black box decides in main(); without it, after every update), or, with the
  // step records, when that boot's furthest step was below 6 (a fault record keeps its step in
  // lastMsgType's high nibble). A store that a boot cannot read is never read twice; the phone
  // pushes its layout again.
  {
    bool wipe = BootConfirm::WipeStoreAtBoot();
#if HA_BOOT_TRACE
    // Only with the step records: without them every record reads step 0.
    if (hasCrashContext_) {
      const uint8_t bc = previousCrashContext_.breadcrumb;
      const uint8_t step = ((bc & 0xF0u) == 0xF0u) ? static_cast<uint8_t>(previousCrashContext_.lastMsgType >> 4)
                                                   : static_cast<uint8_t>(bc >> 4);
      if (step < 6) {
        wipe = true;
      }
    }
#endif
    if (wipe && bleService_.FileSystem() != nullptr) {
      bleService_.FileSystem()->FileDelete(LayoutStoreFlash::kRecordPath);
    }
  }
#endif

#if HA_CRASH_CONTEXT_EXTENDED
  // Record reset reason at boot so fault handlers can see the prior reset cause
  // even if they fire before HELLO is sent.
  if (watchdog_) {
    g_crashContext.lastResetReason =
      static_cast<uint8_t>(watchdog_->GetResetReason());
  } else {
    g_crashContext.lastResetReason = 0;
  }
  // displayStackHwm is sampled on the first UpdateDisplayStackHwm() call from
  // display-task context (REQ-060 wiring); pre-zero it here so the field is
  // never garbage before the first Refresh() fires.
  g_crashContext.displayStackHwm = 0;

#if HA_WATCHDOG_SUBSYSTEM_AWARE
  // REQ-060 — initialise watchdogMask to 0 (no missing subsystems at boot).
  // The heartbeat flags in g_watchdogHeartbeat are zero'd here too (they live
  // in .noinit so we cannot rely on startup zeroing).
  g_crashContext.watchdogMask = 0;
  for (uint8_t i = 0; i < kWatchdogSubsystemCount; ++i) {
    g_watchdogHeartbeat[i] = 0;
  }
#endif // HA_WATCHDOG_SUBSYSTEM_AWARE
  // Zero remaining extended fields so the initial crash context is clean.
  g_crashContext.pc          = 0;
  g_crashContext.lr          = 0;
  g_crashContext.xpsr        = 0;
  g_crashContext.cfsr        = 0;
  g_crashContext.hfsr        = 0;
  g_crashContext.mmfar       = 0;
  g_crashContext.bfar        = 0;
  g_crashContext.taskName[0] = '\0';
  g_crashContext.lvFreeSize    = 0;
  g_crashContext.lvFreeBiggest = 0;
  g_crashContext.activeScreenId = 0;
  g_crashContext.layoutGen      = 0;
  g_crashContext.layoutCrc      = 0;
#endif // HA_CRASH_CONTEXT_EXTENDED

  // REQ-059 — initialise crash-dump ring.
  // Gated on HA_CRASH_CONTEXT_EXTENDED: the ring and its boot-time init are
  // .noinit deltas vs the 2.4.10 binary; disabled while bisecting boot loop.
#if HA_CRASH_CONTEXT_EXTENDED && HA_CRASH_RING
  if (g_crashRing.hdr.magic != kCrashRingMagic) {
    g_crashRing = {};                      // zero-fill all fields
    g_crashRing.hdr.magic = kCrashRingMagic;
    g_crashRing.hdr.head  = 0;
    g_crashRing.hdr.tail  = 0;
    g_crashRing.hdr.count = 0;
  }
#endif // HA_CRASH_CONTEXT_EXTENDED && HA_CRASH_RING

  // Set initial breadcrumb
  SetBreadcrumb(Breadcrumb::IDLE);
}

void BridgeController::OnBleDataReceived(const uint8_t* data, size_t len) {
  SetBreadcrumb(Breadcrumb::BLE_RX);

  // MTU capture (NimBLE host-task context, link is up because we just received
  // a frame): refresh the cached MTU on every inbound frame so any ACK we send
  // in response uses the real negotiated MTU.
  //
  // Bug this fixes: lastKnownMtu_ was previously only updated by the
  // display-task OnTick poll (HomeAssistant::Refresh -> ProcessTick -> OnTick),
  // which runs ONLY while the HA watchface is the active, awake screen. When a
  // phone pushed CONFIG_PUSH / WATCHFACE_LAYOUT / HELLO while the watch sat on
  // another face or with the display asleep, lastKnownMtu_ stayed 0, so
  // SendSingleFrameImpl's guard floored effectiveMtu at 23 (max 20-byte frame)
  // and silently dropped (oversizeNotifyDrops_++) every response larger than
  // ~11 payload bytes: CONFIG_ACK, WATCHFACE_ACK, NOTIFY_ACK and the HELLO
  // diagnostic. Only PONG (9-byte, empty payload) ever got through — which is
  // exactly what the debug bundles showed (PONG the sole watch->phone frame;
  // every ACK timing out; firmware version / heap / breadcrumb always null).
  {
    uint16_t mtu = bleService_.GetCurrentMtu();
    if (mtu > 0 && mtu != lastKnownMtu_) {
      SetMtu(mtu);
    }
  }

  // REQ-011 — producer side of the static mailbox. Runs in NimBLE host-task
  // context. Must not block: if the consumer is behind, drop the frame and
  // increment the overflow counter so the phone can detect lost data via
  // HELLO. Direct-dispatch is only used when the consumer task failed to
  // create (defensive — static allocation should never fail).
#if !HA_BLE_RX_MAILBOX
  // Mailbox compiled out: always direct-dispatch on the NimBLE host task.
  // bleProcessingTask_ is unconditionally null in this configuration, so the
  // producer below was unreachable — keeping it out of the build also keeps
  // the 2112 B ring out of .bss (REQ-011 budget).
  {
    uint32_t nowMs = CodecClockMs();
    SetBreadcrumb(Breadcrumb::FRAME_DECODE);
    frameCodec_.OnDataReceived(data, len, nowMs, [this](const Frame& frame) {
      OnFrameReceived(frame);
    });
    return;
  }
#else
  if (!bleProcessingTask_) {
    uint32_t nowMs = CodecClockMs();
    SetBreadcrumb(Breadcrumb::FRAME_DECODE);
    frameCodec_.OnDataReceived(data, len, nowMs, [this](const Frame& frame) {
      OnFrameReceived(frame);
    });
    return;
  }

  const uint8_t head = bleQueueHead_.load(std::memory_order_relaxed);
  const uint8_t nextHead = static_cast<uint8_t>((head + 1u) % kBleQueueDepth);
  const uint8_t tail = bleQueueTail_.load(std::memory_order_acquire);
  if (nextHead == tail) {
    // Ring full — drop and account. Producer is intentionally never blocked
    // here; NimBLE host task back-pressure would cause connection drops.
    bleQueueOverflows_.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  BleQueueSlot& slot = bleQueueSlots_[head];
  uint16_t itemLen = (len > kBleQueueItemSize) ? kBleQueueItemSize : static_cast<uint16_t>(len);
  std::memcpy(slot.data, data, itemLen);
  slot.len = itemLen;

  // Release the new head so the consumer sees a fully-written slot.
  bleQueueHead_.store(nextHead, std::memory_order_release);

  // Wake the consumer. Binary semaphore: extra gives while the semaphore is
  // already "given" are no-ops, so multiple producer wake-ups before the
  // consumer runs collapse to a single take — same coalescing behavior the
  // previous xTaskNotifyGive path relied on. The consumer drains the whole
  // SPSC ring per wake, so missing a coalesced wake never loses data.
  if (bleWakeSem_) {
    xSemaphoreGive(bleWakeSem_);
  }
#endif // !HA_BLE_RX_MAILBOX
}

#if HA_BLE_RX_MAILBOX
void BridgeController::BleProcessingTaskEntry(void* param) {
  static_cast<BridgeController*>(param)->BleProcessingTaskLoop();
}

void BridgeController::BleProcessingTaskLoop() {
  // REQ-011 — consumer side. Wait for a wake from the producer, then drain
  // every slot that the producer has committed since we last looked. Binary
  // semaphore replaces ulTaskNotifyTake here because InfiniTime sets
  // configUSE_TASK_NOTIFICATIONS=0; coalescing behavior is equivalent (extra
  // gives between takes are absorbed; the inner while-loop empties the ring).
  for (;;) {
    if (bleWakeSem_) {
      xSemaphoreTake(bleWakeSem_, portMAX_DELAY);
    }

    for (;;) {
      const uint8_t tail = bleQueueTail_.load(std::memory_order_relaxed);
      const uint8_t head = bleQueueHead_.load(std::memory_order_acquire);
      if (head == tail) break;  // ring empty

      BleQueueSlot& slot = bleQueueSlots_[tail];
      const uint32_t nowMs = CodecClockMs();
      SetBreadcrumb(Breadcrumb::FRAME_DECODE);
      frameCodec_.OnDataReceived(slot.data, slot.len, nowMs,
        [this](const Frame& frame) { OnFrameReceived(frame); });

      const uint8_t nextTail = static_cast<uint8_t>((tail + 1u) % kBleQueueDepth);
      bleQueueTail_.store(nextTail, std::memory_order_release);
    }

    // REQ-060 — BLE task is alive; kick the BLE subsystem heartbeat after
    // each drain cycle.  ISR-safe byte write (no FreeRTOS primitives).
#if HA_WATCHDOG_SUBSYSTEM_AWARE
    WatchdogSubsystemKick(WatchdogSubsystem::Ble);
#endif
  }
}
#endif // HA_BLE_RX_MAILBOX

// Check CBOR nesting depth by scanning raw bytes.
// Returns true if nesting stays within maxDepth levels.
//
// WHY: tinycbor's cbor_value_advance uses CBOR_PARSER_MAX_RECURSIONS = 1024
// internally. On the firmware's 8KB task stack, 1024 recursion levels would
// overflow (~20-40 bytes per recursion frame). This pre-scan rejects payloads
// with excessive nesting before they reach tinycbor's recursive skip logic.
//
// The scan reads CBOR major-type headers to track container open/close.
// For definite-length containers, it tracks remaining item counts to know
// when a container ends and depth should decrease. For indefinite-length
// containers, depth decreases when the break code (0xFF) is encountered.
// Maps count key-value pairs, so each pair is 2 items for tracking purposes.
static bool CborNestingOk(const uint8_t* data, size_t len, uint8_t maxDepth) {
  // Stack tracks remaining items at each nesting level.
  // Using int32_t: -1 means indefinite-length container.
  // maxDepth + 1 entries to support checking depth > maxDepth before rejecting.
  int32_t remaining[12];  // Supports up to depth 11 (maxDepth=10 + 1 for overflow detection)
  uint8_t depth = 0;
  size_t pos = 0;

  auto consumeItem = [&]() {
    // After processing one complete CBOR data item, decrement the remaining
    // count at the current depth. When a container's items are exhausted, pop it.
    while (depth > 0) {
      if (remaining[depth - 1] == -1) break;  // Indefinite — only closed by break
      remaining[depth - 1]--;
      if (remaining[depth - 1] <= 0) {
        depth--;
      } else {
        break;
      }
    }
  };

  while (pos < len) {
    uint8_t byte = data[pos];
    uint8_t majorType = byte >> 5;
    uint8_t additional = byte & 0x1F;

    if (majorType == 4 || majorType == 5) {
      // Array or map — push new nesting level
      if (depth >= maxDepth) return false;  // Would exceed max depth

      pos++;
      int32_t count = 0;
      bool indefinite = false;

      if (additional <= 23) {
        count = additional;
      } else if (additional == 24 && pos < len) {
        count = data[pos++];
      } else if (additional == 25 && pos + 1 < len) {
        count = (static_cast<int32_t>(data[pos]) << 8) | data[pos + 1];
        pos += 2;
      } else if (additional == 26 && pos + 3 < len) {
        // For depth-checking purposes, cap to prevent overflow
        count = 0x7FFF;  // Large but bounded
        pos += 4;
      } else if (additional == 27 && pos + 7 < len) {
        count = 0x7FFF;
        pos += 8;
      } else if (additional == 31) {
        indefinite = true;
      } else {
        return false;  // Malformed
      }

      if (indefinite) {
        remaining[depth] = -1;
      } else {
        // Maps have key+value per entry, so count * 2 items
        remaining[depth] = (majorType == 5) ? count * 2 : count;
      }
      depth++;

      // Empty container (count=0) is immediately complete
      if (!indefinite && count == 0) {
        depth--;
        consumeItem();
      }

      continue;
    }

    if (byte == 0xFF) {
      // Break code — closes indefinite-length container
      pos++;
      if (depth > 0 && remaining[depth - 1] == -1) {
        depth--;
        consumeItem();
      }
      continue;
    }

    // Non-container data item — skip its encoded bytes
    pos++;
    if (majorType == 0 || majorType == 1) {
      // Unsigned/negative integer
      if (additional == 24) pos++;
      else if (additional == 25) pos += 2;
      else if (additional == 26) pos += 4;
      else if (additional == 27) pos += 8;
    } else if (majorType == 2 || majorType == 3) {
      // Byte/text string — skip the content
      size_t slen = 0;
      if (additional <= 23) {
        slen = additional;
      } else if (additional == 24 && pos < len) {
        slen = data[pos++];
      } else if (additional == 25 && pos + 1 < len) {
        slen = (static_cast<size_t>(data[pos]) << 8) | data[pos + 1];
        pos += 2;
      } else if (additional == 26 && pos + 3 < len) {
        slen = (static_cast<size_t>(data[pos]) << 24) | (static_cast<size_t>(data[pos+1]) << 16) |
               (static_cast<size_t>(data[pos+2]) << 8) | data[pos+3];
        pos += 4;
      } else if (additional == 27) {
        // 8-byte length — skip for safety (payload is capped at 10KB anyway)
        pos += 8;
        slen = len;  // Will clamp below
      } else if (additional == 31) {
        // Indefinite-length string — skip chunks until break
        while (pos < len && data[pos] != 0xFF) {
          uint8_t cb = data[pos++];
          uint8_t ca = cb & 0x1F;
          size_t cl = 0;
          if (ca <= 23) { cl = ca; }
          else if (ca == 24 && pos < len) { cl = data[pos++]; }
          else if (ca == 25 && pos + 1 < len) { cl = (static_cast<size_t>(data[pos]) << 8) | data[pos+1]; pos += 2; }
          else break;
          pos += cl;
        }
        if (pos < len) pos++;  // Skip break
        consumeItem();
        continue;
      }
      pos += slen;
    } else if (majorType == 6) {
      // Tag — skip tag number, the tagged value follows as next item
      if (additional == 24) pos++;
      else if (additional == 25) pos += 2;
      else if (additional == 26) pos += 4;
      else if (additional == 27) pos += 8;
      continue;  // Don't consumeItem — the tagged value is next
    } else if (majorType == 7) {
      // Simple values / floats
      if (additional == 24) pos++;
      else if (additional == 25) pos += 2;
      else if (additional == 26) pos += 4;
      else if (additional == 27) pos += 8;
    }

    consumeItem();
  }
  return true;
}

// Maximum CBOR nesting depth allowed in protocol messages.
// The deepest real message is WATCHFACE_LAYOUT: root map → screens array →
// screen map → dashboard map → statusGroups array → group map → glyphs
// array → glyph map = 8 levels. Allow 10 for safety margin. The value is
// sourced from DashboardLimits::MAX_CBOR_DEPTH (SOT in firmware-limits.yaml)
// so the firmware decoder, HA push_watchface_layout validator, and Android
// preflight all enforce the same bound.
static constexpr uint8_t kMaxCborNestingDepth = DashboardLimits::MAX_CBOR_DEPTH;

void BridgeController::OnFrameReceived(const Frame& frame) {
  // Retry a queued watch-face ACK first, while the host is still in the clean
  // state it is in on entry — before this callback does any decoding of its own.
  FlushPendingWatchFaceAck();

  // Extract payload via zero-copy accessor (borrowed pointer for single-chunk,
  // vector data for multi-chunk). Handlers accept raw pointers.
  const uint8_t* data = frame.PayloadData();
  size_t len = frame.PayloadLen();

  SetBreadcrumb(Breadcrumb::CBOR_DECODE, frame.header.type, frame.header.msgId);

  // Pre-validate CBOR nesting depth before parsing.
  // Rejects payloads with excessive nesting that could overflow the task stack
  // via tinycbor's recursive cbor_value_advance (CBOR_PARSER_MAX_RECURSIONS=1024).
  // Messages without CBOR payloads (PING, PONG) skip this check.
  if (len > 0 && !CborNestingOk(data, len, kMaxCborNestingDepth)) {
    return;  // Reject — excessive nesting depth
  }

  switch (frame.header.type) {
    case static_cast<uint8_t>(MessageType::HELLO):
      HandleHello(data, len, frame.header.msgId);
      break;

    case static_cast<uint8_t>(MessageType::CONFIG_PUSH):
      HandleConfigPush(data, len, frame.header.msgId);
      break;

    case static_cast<uint8_t>(MessageType::STATE_UPDATE):
      HandleStateUpdate(data, len);
      break;

    case static_cast<uint8_t>(MessageType::NOTIFY):
      HandleNotify(data, len, frame.header.msgId);
      break;

    case static_cast<uint8_t>(MessageType::NOTIFY_REMOVE):
      HandleNotifyRemove(data, len);
      break;

    case static_cast<uint8_t>(MessageType::PING):
      HandlePing(frame.header.msgId);
      break;

    case static_cast<uint8_t>(MessageType::HAPTIC_FEEDBACK):
      HandleHapticFeedback(data, len);
      break;

    case static_cast<uint8_t>(MessageType::WATCHFACE_LAYOUT):
#if defined(HA_BUILD_VARIANT_FROZEN_DASH) && HA_BUILD_VARIANT_FROZEN_DASH
      // REQ-006 variant: ignore live layout pushes so the HA face renders only
      // a frozen compiled-in fixture. Used to isolate live-push race effects.
      break;
#else
      HandleWatchFaceLayout(data, len, frame.header.msgId);
      break;
#endif

    case static_cast<uint8_t>(MessageType::TIME_SYNC):
      HandleTimeSync(data, len);
      break;

    case static_cast<uint8_t>(MessageType::LOGBOOK_UPDATE):
      HandleLogbookUpdate(data, len);
      break;

    case static_cast<uint8_t>(MessageType::CALENDAR_UPDATE):
      HandleCalendarUpdate(data, len);
      break;

    case static_cast<uint8_t>(MessageType::HOME_CONTROL_UPDATE):
      HandleHomeControlUpdate(data, len);
      break;

    case static_cast<uint8_t>(MessageType::DASHBOARD_STATE):
      HandleDashboardState(data, len);
      break;

    case static_cast<uint8_t>(MessageType::HELLO_ACK):
      HandleHelloAck(data, len);
      break;

    // REQ-059 — crash-dump retrieval handshake: the 4-slot ring with
    // HA_CRASH_RING, a ring of one without it (kFeatureCrashReport).
    case static_cast<uint8_t>(MessageType::CRASH_DUMP_REQUEST):
      HandleCrashDumpRequest(data, len, frame.header.msgId);
      break;

    case static_cast<uint8_t>(MessageType::CRASH_DUMP_ACK):
      HandleCrashDumpAck(data, len);
      break;

    // REQ-072 — find-device round-trip.
    case static_cast<uint8_t>(MessageType::FIND_WATCH):
      HandleFindWatch(data, len, frame.header.msgId);
      break;

    // REQ-068 — action result feedback (phone → watch).
    case static_cast<uint8_t>(MessageType::ACTION_RESULT):
      HandleActionResult(data, len);
      break;

    default:
      break;
  }
}

void BridgeController::HandleHello(const uint8_t* payload, size_t len, uint16_t msgId) {
  (void)payload;
  (void)len;
  (void)msgId;

  // The HELLO reply carries everything the phone decides on, not only telemetry:
  //   - `features`: without it the phone never saw the short-key bit and sent
  //     every layout with long keys (2.4.62 and 2.4.63);
  //   - the counters the hardware gate scores (mallocFails, stackOverflows,
  //     uptimeTicks for reset continuity, safeFaceTrips, lvCreateFailures,
  //     layoutRefused, layoutWriteFailed);
  //   - bridgeProtocolRev / helloDiagRev, which tell the phone which of those
  //     fields to expect.
  // There is NO chunked TX path: SendSingleFrameImpl rejects a frame larger than
  // its 256-byte frame buffer or than (MTU-3). At the 256-byte ATT MTU that caps
  // the payload at 244 bytes, so the map has to fit in that. The 15 fields below
  // (2.4.101 added "enc", 5 bytes; 2.4.103's bit 16 widens "features" from 3 bytes to 5)
  // are about 224 bytes in practice. 2.4.104's optional 16th, "wk", is up to 19 bytes more and goes
  // in only when the result fits (HelloPayloadRoom: the 256-byte frame buffer and the MTU-3 the link
  // carries, less the 9-byte header; 244 at the 256-byte MTU). Uncapped, the worst case (the counters at 32 bits, a
  // 12-character version) is 257 bytes, which overflows the buffer and drops the HELLO, so
  // the six counters and the two heap values are capped at 0xFFFF in the HELLO only (the
  // internal counters keep counting; a heap figure never exceeds the 64 KB of RAM, so its
  // cap changes nothing). A capped value is 3 bytes as a CBOR unsigned int. Worst case after
  // the caps, entry by entry (key bytes + value bytes):
  //   map header 1 | device 7+6=13 | features (5-byte uint) 9+5=14 | fwVersion, 12 chars 10+13=23
  //   | bridgeProtocolRev 18+1=19 | helloDiagRev 13+1=14 | freeHeap 9+3=12 | minFreeHeap 12+3=15
  //   | mallocFails 12+3=15 | stackOverflows 15+3=18 | uptimeTicks (32-bit) 12+5=17
  //   | lvCreateFailures 17+3=20 | safeFaceTrips 14+3=17 | layoutRefused 14+3=17
  //   | layoutWriteFailed 18+3=21 | enc 4+1=5
  //   = 1+13+14+23+19+14+12+15+15+18+17+20+17+17+21+5 = 241 bytes, within the 244.
  // (Until 2.4.98 a richer map was tried first; at about 300 bytes it never fit, and this one is
  // what the phone always got.)
  //
  // helloDiagRev is an explicit capability marker.  Android only treats a
  // HELLO field as meaningful when helloDiagRev >= the rev that introduced
  // that field, which removes the "is the field missing because of old
  // firmware, a decode bug, or a partial implementation?" ambiguity.
  //
  // Concurrency: this buffer is reentrant-unsafe.  HandleHello is only invoked
  // from the single BLE-processing task (see BridgeController::ProcessBleQueue);
  // if that invariant is ever broken, the static storage will corrupt silently.
  // 256 B holds the largest the 15 fields can be (241 B, above) and is no more than a
  // frame can carry anyway. The buffer is
  // static, so it comes out of .bss and, through the heap sizing, the runtime
  // heap: it was 768 B for the richer map that never fit.
  static uint8_t buffer[256];
  // Caps a value at 0xFFFF for the HELLO, so it is at most 3 bytes on the wire.
  const auto cap16 = [](uint32_t v) -> uint32_t { return std::min<uint32_t>(v, 0xFFFFu); };
  // 2.4.104 (helloDiagRev 5): the screen-wake telemetry, one more key ("wk", WakeDiag.h, at most
  // 19 bytes), only when the whole map still fits the frame the link carries. The frame limit is
  // what SendSingleFrameImpl enforces, so a HELLO that fits here is sent, and one that does not
  // goes out as the 15-field map, exactly as before.
  const HaBridge::WakeDiag wake = bleService_.ReadWakeDiag();
  const size_t payloadRoom = HaBridge::HelloPayloadRoom(lastKnownMtu_);
  size_t extraNeeded = 0;
  size_t helloLen = 0;
  // In the function itself, not a lambda: the release's post guard reads the feature word from this
  // function's own code.
  bool withWake = true;
  for (;;) {
    CborEncoder cEnc, cMap;
    cbor_encoder_init(&cEnc, buffer, sizeof(buffer), 0);
    cbor_encoder_create_map(&cEnc, &cMap, withWake ? 16 : 15);
    cbor_encode_text_stringz(&cMap, "device");                cbor_encode_text_stringz(&cMap, "watch");
    cbor_encode_text_stringz(&cMap, "features");              cbor_encode_uint(&cMap, kSupportedFeatures);
    cbor_encode_text_stringz(&cMap, "fwVersion");             cbor_encode_text_stringz(&cMap, Pinetime::Version::VersionString());
    cbor_encode_text_stringz(&cMap, "bridgeProtocolRev");     cbor_encode_uint(&cMap, DashboardLimits::BRIDGE_PROTOCOL_REV);
    cbor_encode_text_stringz(&cMap, "helloDiagRev");          cbor_encode_uint(&cMap, DashboardLimits::HELLO_DIAG_REV);
    cbor_encode_text_stringz(&cMap, "freeHeap");              cbor_encode_uint(&cMap, cap16(xPortGetFreeHeapSize()));
    cbor_encode_text_stringz(&cMap, "minFreeHeap");           cbor_encode_uint(&cMap, cap16(xPortGetMinimumEverFreeHeapSize()));
    cbor_encode_text_stringz(&cMap, "mallocFails");           cbor_encode_uint(&cMap, cap16(static_cast<uint32_t>(mallocFailedCount)));
    cbor_encode_text_stringz(&cMap, "stackOverflows");        cbor_encode_uint(&cMap, cap16(static_cast<uint32_t>(stackOverflowCount)));
    cbor_encode_text_stringz(&cMap, "uptimeTicks");           cbor_encode_uint(&cMap, xTaskGetTickCount());
    cbor_encode_text_stringz(&cMap, "lvCreateFailures");      cbor_encode_uint(&cMap, cap16(bleService_.GetLvCreateFailures()));
    cbor_encode_text_stringz(&cMap, "safeFaceTrips");         cbor_encode_uint(&cMap, cap16(bleService_.GetSafeFaceTrips()));
    cbor_encode_text_stringz(&cMap, "layoutRefused");         cbor_encode_uint(&cMap, cap16(LayoutRefusedCount()));
    cbor_encode_text_stringz(&cMap, "layoutWriteFailed");     cbor_encode_uint(&cMap, cap16(LayoutFailedCount()));
    // 2.4.101: whether this link is encrypted. The phone sends notification content only when it is.
    EncodeEncPair(&cMap, bleService_.IsLinkEncrypted());
    if (withWake) {
      HaBridge::EncodeWakeDiag(&cMap, wake);
    }
    cbor_encoder_close_container(&cEnc, &cMap);
    extraNeeded = cbor_encoder_get_extra_bytes_needed(&cEnc);
    helloLen = cbor_encoder_get_buffer_size(&cEnc, buffer);
    if (!withWake || (extraNeeded == 0 && helloLen <= payloadRoom)) {
      break;
    }
    withWake = false;  // too long for the frame: the 15 fields, as every earlier firmware sent
  }

  // NFR-1: silent-truncation guard.  TinyCBOR keeps counting after an overflow
  // instead of failing; if the buffer was too small the emitted bytes are a
  // corrupt prefix, so send nothing and leave the breadcrumb for the crash report.
  if (extraNeeded != 0) {
    SetBreadcrumb(Breadcrumb::CBOR_ENCODE,
                  static_cast<uint8_t>(MessageType::HELLO),
                  static_cast<uint16_t>(std::min(extraNeeded,
                                                 static_cast<size_t>(UINT16_MAX))));
    return;
  }
  SendFromBle(MessageType::HELLO, buffer, helloLen);

  // REQ-057: hasCrashContext_ is NOT cleared here, and this HELLO does not carry
  // the crash context: the phone fetches it with CRASH_DUMP_REQUEST.  It is cleared
  // only when the phone confirms it has the report (HandleHelloAck with
  // crashContextAck=true, or CRASH_DUMP_ACK), so a dropped notification or a
  // disconnect does not lose it.

  // Reset breadcrumb to IDLE after successful HELLO handshake
  SetBreadcrumb(Breadcrumb::IDLE);
}

void BridgeController::HandleHelloAck(const uint8_t* payload, size_t len) {
  // Parse HELLO_ACK from the companion app. The only key read is "crashContextAck"
  // (bool, REQ-057); the rest of the map (the phone's protocol version, its
  // features) is skipped.
  //
  // REQ-057: when "crashContextAck" is true the phone has ingested the crash
  // context. Only then do we clear hasCrashContext_. If the field is absent (older
  // phone firmware) hasCrashContext_ is left set, and the crash dump reply keeps
  // carrying it until an ack-capable phone acknowledges it.
  SetBreadcrumb(Breadcrumb::CBOR_DECODE, static_cast<uint8_t>(MessageType::HELLO_ACK), 0);

  CborParser parser;
  CborValue root, mapValue;
  if (cbor_parser_init(payload, len, 0, &parser, &root) != CborNoError || !cbor_value_is_map(&root)) {
    return;
  }

  bool crashContextAck = false;
  if (cbor_value_enter_container(&root, &mapValue) != CborNoError) return;

  char keyBuf[20];
  while (!cbor_value_at_end(&mapValue)) {
    if (!cbor_value_is_text_string(&mapValue)) { cbor_value_advance(&mapValue); continue; }
    size_t keyLen = sizeof(keyBuf) - 1;
    if (cbor_value_copy_text_string(&mapValue, keyBuf, &keyLen, &mapValue) != CborNoError) break;
    keyBuf[keyLen] = '\0';

    if (strcmp(keyBuf, "crashContextAck") == 0 && cbor_value_is_boolean(&mapValue)) {
      // REQ-057: phone confirmed crash context delivery.
      bool ack = false;
      cbor_value_get_boolean(&mapValue, &ack);
      crashContextAck = ack;
    }
    cbor_value_advance(&mapValue);
  }

  // REQ-057: clear crash context only after confirmed phone ingestion.
  if (crashContextAck && hasCrashContext_) {
    hasCrashContext_ = false;
  }

  SetBreadcrumb(Breadcrumb::IDLE);
}

// ---------------------------------------------------------------------------
// REQ-059 — CRASH_DUMP_REQUEST / CRASH_DUMP_ACK handlers.
//
// The 4-slot ring is gated on HA_CRASH_CONTEXT_EXTENDED && HA_CRASH_RING: it
// was disabled while the 2.4.11+ boot loop was bisected (a .noinit delta vs
// 2.4.10). Without it, the #else branch below serves a ring of one.
// ---------------------------------------------------------------------------
#if HA_CRASH_CONTEXT_EXTENDED && HA_CRASH_RING

void BridgeController::HandleCrashDumpRequest(const uint8_t* payload, size_t len,
                                               uint16_t msgId) {
  (void)payload;
  (void)len;

  SetBreadcrumb(Breadcrumb::CBOR_ENCODE,
                static_cast<uint8_t>(MessageType::CRASH_DUMP_REQUEST), msgId);

  // Static encode buffer for crash dump response.  Max size: CBOR map overhead
  // (≈30 B) + same extended crashContext fields as HELLO (≈265 B when extended)
  // + a few extra bytes for the slot-index field.  768 B is comfortably safe.
  static uint8_t buf[768];
  CborEncoder enc, map;
  cbor_encoder_init(&enc, buf, sizeof(buf), 0);

  if (g_crashRing.hdr.magic != kCrashRingMagic || g_crashRing.hdr.count == 0) {
    // Ring not initialised or empty — reply with {"empty": true}.
    cbor_encoder_create_map(&enc, &map, 1);
    cbor_encode_text_stringz(&map, "empty");
    cbor_encode_boolean(&map, true);
    cbor_encoder_close_container(&enc, &map);
  } else {
    // Return the tail slot (oldest undelivered entry).
    const uint8_t tailIdx       = g_crashRing.hdr.tail;
    const CrashContext& slot    = g_crashRing.slots[tailIdx];

    // Field count: slotIndex (1) + base (5) + extended (15) = 21 when extended.
    // REQ-060: +1 watchdogMask when HA_WATCHDOG_SUBSYSTEM_AWARE also ON → 22.
#if HA_CRASH_CONTEXT_EXTENDED && HA_WATCHDOG_SUBSYSTEM_AWARE
    cbor_encoder_create_map(&enc, &map, 22);
#elif HA_CRASH_CONTEXT_EXTENDED
    cbor_encoder_create_map(&enc, &map, 21);
#else
    cbor_encoder_create_map(&enc, &map, 6);
#endif
    cbor_encode_text_stringz(&map, "slotIndex");
    cbor_encode_uint(&map, tailIdx);

    // Base fields
    cbor_encode_text_stringz(&map, "breadcrumb");
    cbor_encode_uint(&map, slot.breadcrumb);
    cbor_encode_text_stringz(&map, "lastMsgType");
    cbor_encode_uint(&map, slot.lastMsgType);
    cbor_encode_text_stringz(&map, "lastMsgId");
    cbor_encode_uint(&map, slot.lastMsgId);
    cbor_encode_text_stringz(&map, "freeHeap");
    cbor_encode_uint(&map, slot.freeHeapAtCrash);
    cbor_encode_text_stringz(&map, "uptimeTicks");
    cbor_encode_uint(&map, slot.uptimeTicksAtCrash);

#if HA_CRASH_CONTEXT_EXTENDED
    // Extended fault fields (REQ-058)
    cbor_encode_text_stringz(&map, "pc");
    cbor_encode_uint(&map, slot.pc);
    cbor_encode_text_stringz(&map, "lr");
    cbor_encode_uint(&map, slot.lr);
    cbor_encode_text_stringz(&map, "xpsr");
    cbor_encode_uint(&map, slot.xpsr);
    cbor_encode_text_stringz(&map, "cfsr");
    cbor_encode_uint(&map, slot.cfsr);
    cbor_encode_text_stringz(&map, "hfsr");
    cbor_encode_uint(&map, slot.hfsr);
    cbor_encode_text_stringz(&map, "mmfar");
    cbor_encode_uint(&map, slot.mmfar);
    cbor_encode_text_stringz(&map, "bfar");
    cbor_encode_uint(&map, slot.bfar);
    cbor_encode_text_stringz(&map, "taskName");
    cbor_encode_text_stringz(&map, slot.taskName);
    cbor_encode_text_stringz(&map, "displayStackHwm");
    cbor_encode_uint(&map, slot.displayStackHwm);
    cbor_encode_text_stringz(&map, "lvFreeSize");
    cbor_encode_uint(&map, slot.lvFreeSize);
    cbor_encode_text_stringz(&map, "lvFreeBiggest");
    cbor_encode_uint(&map, slot.lvFreeBiggest);
    cbor_encode_text_stringz(&map, "activeScreenId");
    cbor_encode_uint(&map, slot.activeScreenId);
    cbor_encode_text_stringz(&map, "lastResetReason");
    cbor_encode_uint(&map, slot.lastResetReason);
    cbor_encode_text_stringz(&map, "layoutGen");
    cbor_encode_uint(&map, slot.layoutGen);
    cbor_encode_text_stringz(&map, "layoutCrc");
    cbor_encode_uint(&map, slot.layoutCrc);

#if HA_WATCHDOG_SUBSYSTEM_AWARE
    // REQ-060 — missing-heartbeat mask at last watchdog check.
    cbor_encode_text_stringz(&map, "watchdogMask");
    cbor_encode_uint(&map, slot.watchdogMask);
#endif // HA_WATCHDOG_SUBSYSTEM_AWARE
#endif // HA_CRASH_CONTEXT_EXTENDED

    cbor_encoder_close_container(&enc, &map);
  }

  // Overflow guard: if the encode overflowed, bail silently — the phone will
  // retry with another CRASH_DUMP_REQUEST on the next connection.
  if (cbor_encoder_get_extra_bytes_needed(&enc) != 0) {
    SetBreadcrumb(Breadcrumb::IDLE);
    return;
  }

  const size_t outLen = cbor_encoder_get_buffer_size(&enc, buf);
  // Response is sent as a CRASH_DUMP_REQUEST reply (same type, watch → phone).
  // The phone matches the response by message type; msgId is not echoed because
  // CRASH_DUMP_REQUEST/ACK are a simple poll-and-confirm pattern, not a
  // request-response pair tracked by msgId.
  SendFromBle(MessageType::CRASH_DUMP_REQUEST, buf, outLen);

  SetBreadcrumb(Breadcrumb::IDLE);
}

void BridgeController::HandleCrashDumpAck(const uint8_t* payload, size_t len) {
  (void)payload;
  (void)len;

  SetBreadcrumb(Breadcrumb::CBOR_DECODE, static_cast<uint8_t>(MessageType::CRASH_DUMP_ACK), 0);

  // Mark the tail slot as delivered: advance tail and decrement count.
  // Silently ignore if the ring is uninitialised or empty (idempotent).
  if (g_crashRing.hdr.magic == kCrashRingMagic && g_crashRing.hdr.count > 0) {
    g_crashRing.hdr.tail = static_cast<uint8_t>(
      (g_crashRing.hdr.tail + 1u) % kCrashRingSlots);
    g_crashRing.hdr.count--;
  }

  SetBreadcrumb(Breadcrumb::IDLE);
}

#else // !(HA_CRASH_CONTEXT_EXTENDED && HA_CRASH_RING)

// Without the ring: a ring of one (kFeatureCrashReport). Its slot is the context
// captured at boot, previousCrashContext_, and the reply has the ring's format,
// so a phone reads both alike: {"empty": true} when there is none, else
// slotIndex 0 and the five base fields. Both forms add resetReason. The rich
// HELLO is the only other carrier of either, and it never fits a packet (see
// HandleHello), so this is how they reach the phone. From 2.4.90 a "box" map
// carries the boot black box's record of how the previous boot ended, when it
// ended badly (a fault, an SDK error, the watchdog, a lockup): what the watch
// shows on its screen for a minute after such a boot. About 200 B at most, under
// the 244 B of one frame. The buffer is on the NimBLE host task's stack (2880 B;
// the worst static path from OnCommand is 1608 B): a static one would come out of
// the FreeRTOS heap.
void BridgeController::HandleCrashDumpRequest(const uint8_t* payload, size_t len,
                                               uint16_t msgId) {
  (void)payload;
  (void)len;

  SetBreadcrumb(Breadcrumb::CBOR_ENCODE,
                static_cast<uint8_t>(MessageType::CRASH_DUMP_REQUEST), msgId);

  HaBootBoxCrash box {};
  const bool haveBox = HaBootBoxGetCrash(&box);

  uint8_t buffer[256];
  CborEncoder enc, map;
  cbor_encoder_init(&enc, buffer, sizeof(buffer), 0);
  cbor_encoder_create_map(&enc, &map, (hasCrashContext_ ? 7 : 2) + (haveBox ? 1 : 0));
  cbor_encode_text_stringz(&map, "resetReason");
  cbor_encode_text_stringz(&map, watchdog_ != nullptr
                                   ? Pinetime::Drivers::ResetReasonToString(watchdog_->GetResetReason())
                                   : "unknown");
  if (hasCrashContext_) {
    cbor_encode_text_stringz(&map, "slotIndex");    cbor_encode_uint(&map, 0);
    cbor_encode_text_stringz(&map, "breadcrumb");   cbor_encode_uint(&map, previousCrashContext_.breadcrumb);
    cbor_encode_text_stringz(&map, "lastMsgType");  cbor_encode_uint(&map, previousCrashContext_.lastMsgType);
    cbor_encode_text_stringz(&map, "lastMsgId");    cbor_encode_uint(&map, previousCrashContext_.lastMsgId);
    cbor_encode_text_stringz(&map, "freeHeap");     cbor_encode_uint(&map, previousCrashContext_.freeHeapAtCrash);
    cbor_encode_text_stringz(&map, "uptimeTicks");  cbor_encode_uint(&map, previousCrashContext_.uptimeTicksAtCrash);
  } else {
    cbor_encode_text_stringz(&map, "empty");
    cbor_encode_boolean(&map, true);
  }
  if (haveBox) {
    CborEncoder boxMap, calls;
    cbor_encode_text_stringz(&map, "box");
    cbor_encoder_create_map(&map, &boxMap, 10);
    cbor_encode_text_stringz(&boxMap, "k");  cbor_encode_uint(&boxMap, box.kind);
    cbor_encode_text_stringz(&boxMap, "c");  cbor_encode_uint(&boxMap, box.ctx);
    cbor_encode_text_stringz(&boxMap, "s");  cbor_encode_uint(&boxMap, box.stage);
    cbor_encode_text_stringz(&boxMap, "u");  cbor_encode_uint(&boxMap, box.uptimeTicks);
    cbor_encode_text_stringz(&boxMap, "pc"); cbor_encode_uint(&boxMap, box.pc);
    cbor_encode_text_stringz(&boxMap, "lr"); cbor_encode_uint(&boxMap, box.lr);
    cbor_encode_text_stringz(&boxMap, "cf"); cbor_encode_uint(&boxMap, box.cfsr);
    cbor_encode_text_stringz(&boxMap, "ad"); cbor_encode_uint(&boxMap, box.addr);
    cbor_encode_text_stringz(&boxMap, "h");  cbor_encode_uint(&boxMap, box.minHeap);
    cbor_encode_text_stringz(&boxMap, "cl");
    cbor_encoder_create_array(&boxMap, &calls, 2);
    cbor_encode_uint(&calls, box.calls[0]);
    cbor_encode_uint(&calls, box.calls[1]);
    cbor_encoder_close_container(&boxMap, &calls);
    cbor_encoder_close_container(&map, &boxMap);
  }
  cbor_encoder_close_container(&enc, &map);

  // Replied as CRASH_DUMP_REQUEST (watch → phone), as the ring does.
  if (cbor_encoder_get_extra_bytes_needed(&enc) == 0) {
    SendFromBle(MessageType::CRASH_DUMP_REQUEST, buffer, cbor_encoder_get_buffer_size(&enc, buffer));
  }

  SetBreadcrumb(Breadcrumb::IDLE);
}

// The phone has the report: stop offering it, here and in the HELLO. The next
// request reads {"empty": true} until the watch resets again.
void BridgeController::HandleCrashDumpAck(const uint8_t* payload, size_t len) {
  (void)payload;
  (void)len;
  hasCrashContext_ = false;
  HaBootBoxAckCrash();
}

#endif // HA_CRASH_CONTEXT_EXTENDED && HA_CRASH_RING

void BridgeController::HandleConfigPush(const uint8_t* payload, size_t len, uint16_t msgId) {
  (void)msgId;
  // Inline CBOR decode — ZERO heap allocation.
  // WHY: CONFIG_PUSH arrives on every reconnect. The old code called
  // ConfigPushMessage::DecodeCbor which allocated ~40+ heap blocks via
  // std::vector<HaAction>/std::vector<HaEntity> + std::string fields.
  // After multiple reconnects (or an app crash+reconnect), heap fragmentation
  // caused pvPortMalloc to return nullptr inside std::string assignment
  // → HardFault → watchdog reset ("locks up a few seconds then reboots").
  // The actions list is skipped (no screen shows it).
  // The entities are skipped as well (2.4.94). No screen shows them: they filled the entity
  // table ahead of the tile states, the table's only reader, and pushed those out.
  CborParser parser;
  CborValue root, mapIter;

  const auto invalidCfg = [&]() { SendConfigAck(false, 0); };

  if (cbor_parser_init(payload, len, 0, &parser, &root) != CborNoError || !cbor_value_is_map(&root) ||
      cbor_value_enter_container(&root, &mapIter) != CborNoError) {
    invalidCfg();
    return;
  }

  int configVersion = 0;
  bool infinassistantEnabled = false;
  int intervalSec = 60;
  bool voiceAssist = false;  // top-level "voiceAssist"; absent = off

  while (!cbor_value_at_end(&mapIter)) {
    if (!cbor_value_is_text_string(&mapIter)) {
      if (!SkipUnknownEntry(&mapIter)) { invalidCfg(); return; }
      continue;
    }

    bool isConfigVersion = false, isEntities = false, isInfinassistant = false, isVoiceAssist = false;
    cbor_value_text_string_equals(&mapIter, "configVersion", &isConfigVersion);
    cbor_value_text_string_equals(&mapIter, "entities", &isEntities);
    cbor_value_text_string_equals(&mapIter, kConfigKeyInfinassistant, &isInfinassistant);
    cbor_value_text_string_equals(&mapIter, "voiceAssist", &isVoiceAssist);
    cbor_value_advance(&mapIter);  // advance past key

    if (cbor_value_at_end(&mapIter)) break;

    if (isConfigVersion) {
      int64_t v = 0;
      if (cbor_value_get_int64(&mapIter, &v) == CborNoError) {
        configVersion = static_cast<int>(v);
      }
      cbor_value_advance(&mapIter);

    } else if (isVoiceAssist) {
      // Not a bool (a malformed key) reads as off; the value is skipped either way.
      if (cbor_value_is_boolean(&mapIter)) {
        cbor_value_get_boolean(&mapIter, &voiceAssist);
      }
      if (cbor_value_advance(&mapIter) != CborNoError) { invalidCfg(); return; }

    } else if (isEntities) {
      // Skipped whole, container and all (see above).
      if (cbor_value_advance(&mapIter) != CborNoError) { invalidCfg(); return; }

    } else if (isInfinassistant) {
      if (!cbor_value_is_map(&mapIter)) {
        cbor_value_advance(&mapIter);
        continue;
      }
      CborValue infMap;
      if (cbor_value_enter_container(&mapIter, &infMap) == CborNoError) {
        while (!cbor_value_at_end(&infMap)) {
          if (!cbor_value_is_text_string(&infMap)) {
            if (!SkipUnknownEntry(&infMap)) { invalidCfg(); return; }
            continue;
          }
          bool isEnabled = false, isInterval = false;
          cbor_value_text_string_equals(&infMap, "enabled", &isEnabled);
          cbor_value_text_string_equals(&infMap, "sensorReportIntervalSeconds", &isInterval);
          cbor_value_advance(&infMap);  // advance past key
          if (cbor_value_at_end(&infMap)) break;
          if (isEnabled) {
            if (cbor_value_is_boolean(&infMap)) {
              cbor_value_get_boolean(&infMap, &infinassistantEnabled);
            }
            cbor_value_advance(&infMap);
          } else if (isInterval) {
            if (cbor_value_is_integer(&infMap)) {
              int64_t iv = 0;
              if (cbor_value_get_int64(&infMap, &iv) == CborNoError) {
                intervalSec = static_cast<int>(iv);
              }
            }
            cbor_value_advance(&infMap);
          } else {
            cbor_value_advance(&infMap);  // skip unknown field value
          }
        }
        cbor_value_leave_container(&mapIter, &infMap);
      } else {
        cbor_value_advance(&mapIter);
      }

    } else {
      // Skip "actions" and any other unknown key values (including containers)
      cbor_value_advance(&mapIter);
    }
  }

  SetBreadcrumb(Breadcrumb::CONFIG_APPLY, static_cast<uint8_t>(MessageType::CONFIG_PUSH), msgId);

  // Apply infinassistant settings
  infinassistantEnabled_ = infinassistantEnabled;
  if (intervalSec <= 0) intervalSec = 60;
  // Wall-clock ms, not tick counts — see BridgeController.h.
  sensorReportIntervalMs_ = static_cast<uint32_t>(intervalSec) * 1000u;
  if (infinassistantEnabled_) {
    lastSensorReportMs_ = NowMs();
  }

  // The double-click is the voice trigger only while the phone says voice is on.
  voiceEnabled_.store(voiceAssist, std::memory_order_release);

  SendConfigAck(true, configVersion);
}

void BridgeController::HandleStateUpdate(const uint8_t* payload, size_t len) {
  // Inline CBOR decode — ZERO heap allocation.
  // WHY: STATE_UPDATE arrives every 30-60 seconds. The old code created a
  // StateUpdateMessage with std::vector<HaEntity> + std::string fields,
  // causing ~10 heap alloc/free cycles per message. Over 1 hour (~60-120 messages),
  // this fragmented the heap until operator new failed on user interaction.
  // This inline version decodes directly into stack char arrays and calls
  // UpdateEntity() immediately — no temporary objects, no heap allocation.
  CborParser parser;
  CborValue root, rootMap;

  if (cbor_parser_init(payload, len, 0, &parser, &root) != CborNoError) return;
  if (!cbor_value_is_map(&root)) return;
  if (cbor_value_enter_container(&root, &rootMap) != CborNoError) return;

  // Top-level flags: ha_offline and data_stale.
  // Set before processing items so the display can react immediately.
  bool haOffline = false;
  bool dataStale = false;

  while (!cbor_value_at_end(&rootMap)) {
    if (!cbor_value_is_text_string(&rootMap)) {
      if (!SkipUnknownEntry(&rootMap)) return;
      continue;
    }

    bool isItems = false, isHaOffline = false, isDataStale = false;
    cbor_value_text_string_equals(&rootMap, "items", &isItems);
    cbor_value_text_string_equals(&rootMap, "ha_offline", &isHaOffline);
    cbor_value_text_string_equals(&rootMap, "data_stale", &isDataStale);
    cbor_value_advance(&rootMap);

    if (cbor_value_at_end(&rootMap)) break;

    if (isHaOffline) {
      if (cbor_value_is_boolean(&rootMap)) {
        cbor_value_get_boolean(&rootMap, &haOffline);
      }
      cbor_value_advance(&rootMap);
      continue;
    }

    if (isDataStale) {
      if (cbor_value_is_boolean(&rootMap)) {
        cbor_value_get_boolean(&rootMap, &dataStale);
      }
      cbor_value_advance(&rootMap);
      continue;
    }

    if (!isItems || !cbor_value_is_array(&rootMap)) {
      if (!cbor_value_at_end(&rootMap) && cbor_value_advance(&rootMap) != CborNoError) return;
      continue;
    }

    CborValue itemsArray;
    if (cbor_value_enter_container(&rootMap, &itemsArray) != CborNoError) return;

    while (!cbor_value_at_end(&itemsArray)) {
      if (!cbor_value_is_map(&itemsArray)) {
        cbor_value_advance(&itemsArray);
        continue;
      }

      char entityId[64] = {};
      char state[32] = {};

      CborValue itemMap;
      if (cbor_value_enter_container(&itemsArray, &itemMap) != CborNoError) {
        cbor_value_advance(&itemsArray);
        continue;
      }

      while (!cbor_value_at_end(&itemMap)) {
        if (!cbor_value_is_text_string(&itemMap)) {
          if (!SkipUnknownEntry(&itemMap)) return;
          continue;
        }

        bool match = false;

        cbor_value_text_string_equals(&itemMap, "entityId", &match);
        if (match) {
          cbor_value_advance(&itemMap);
          DecodeStringInto(&itemMap, entityId, sizeof(entityId));
          continue;
        }

        cbor_value_text_string_equals(&itemMap, "state", &match);
        if (match) {
          cbor_value_advance(&itemMap);
          DecodeStringInto(&itemMap, state, sizeof(state));
          continue;
        }

        // Unknown key (the unit and the timestamp too: no screen shows them) — skip key and value
        if (!SkipUnknownEntry(&itemMap)) return;
      }

      cbor_value_leave_container(&itemsArray, &itemMap);

      if (entityId[0] != '\0') {
        bleService_.UpdateEntity(entityId, state);
      }
    }

    cbor_value_leave_container(&rootMap, &itemsArray);
  }

  // Update stale flag after processing all fields.
  // ha_offline=true OR data_stale=true → mark data stale.
  // Only a clean update (both false) clears the flag.
  bleService_.SetDataStale(haOffline || dataStale);
}

void BridgeController::HandleNotify(const uint8_t* payload, size_t len, uint16_t msgId) {
  // Inline CBOR decode — ZERO heap allocation.
  // The old code called NotifyMessage::DecodeCbor which allocated 4 std::string objects
  // (title, body, severity, tag). After repeated reconnects, these heap allocs cause
  // fragmentation → malloc failure → HardFault. This version decodes directly into stack
  // buffers (NotifyFields, 178 bytes — safe for NimBLE host task), and the notification goes
  // to InfiniTime's own (HomeAssistantService::PushNotification): the popup, the vibration
  // and the swipe-down list. Until 2.4.95 it went to the HA Notifications app's list.
  // A phone notification carries a nid (2.4.101): the list entry is tied to it there.
  NotifyFields fields;
  if (!DecodeNotify(payload, len, fields)) return;

  bleService_.PushNotification(fields.title, fields.body, fields.nid);
  SendNotifyAck(msgId, fields.tag);
}

void BridgeController::HandleNotifyRemove(const uint8_t* payload, size_t len) {
  // The phone removed the notification (read, opened, cleared). The entry goes silently if it is
  // still there, and nothing is sent back: a missing nid, one already dismissed on the watch or
  // pushed out of the list, all end the same way.
  const uint16_t nid = DecodeNotifyRemove(payload, len);
  if (nid == 0) return;
  bleService_.RemoveNotification(nid);
}

void BridgeController::HandlePing(uint16_t msgId) {
  // Echo the PING's msgId back in the PONG payload.
  //
  // Every outgoing frame gets its own sequential id from
  // frameCodec_.GetNextMsgId() (SendSingleFrameImpl), so the frame header
  // CANNOT carry the request id. The phone's liveness check matches the PONG
  // against the id it sent (REQ-036/037), and with nothing echoed that match
  // could never succeed: every liveness round timed out and the watch showed
  // as unresponsive while the link was healthy.
  //
  // The id goes in the CBOR payload, which is the convention the rest of this
  // file already uses for correlation (see SendNotifyAck).
  //
  // "enc" (2.4.101): whether this link is encrypted, at this moment (LinkReport.h). The whole
  // map is at most 13 bytes (a 16-bit rid), within the 16-byte buffer.
  static uint8_t buffer[16];
  const size_t len = EncodePongPayload(msgId, bleService_.IsLinkEncrypted(), buffer, sizeof(buffer));
  if (len == 0) {
    SendFromBle(MessageType::PONG, nullptr, 0);
    return;
  }
  SendFromBle(MessageType::PONG, buffer, len);
}

void BridgeController::HandleHapticFeedback(const uint8_t* payload, size_t len) {
  if (!motorController_) return;

  // Inline CBOR decode — ZERO heap allocation.
  // Eliminates HapticFeedbackMessage with std::string pattern.
  CborParser parser;
  CborValue root, mapIter;

  if (cbor_parser_init(payload, len, 0, &parser, &root) != CborNoError) return;
  if (!cbor_value_is_map(&root)) return;
  if (cbor_value_enter_container(&root, &mapIter) != CborNoError) return;

  char pattern[24] = {};

  while (!cbor_value_at_end(&mapIter)) {
    if (!cbor_value_is_text_string(&mapIter)) {
      if (!SkipUnknownEntry(&mapIter)) return;
      continue;
    }

    bool isPattern = false;
    cbor_value_text_string_equals(&mapIter, "pattern", &isPattern);
    cbor_value_advance(&mapIter);

    if (cbor_value_at_end(&mapIter)) break;

    if (isPattern) {
      DecodeStringInto(&mapIter, pattern, sizeof(pattern));
    } else {
      cbor_value_advance(&mapIter);
    }
  }

  if (std::strcmp(pattern, "short") == 0) {
    RequestHaptic(HapticPattern::Short);
  } else if (std::strcmp(pattern, "double") == 0) {
    RequestHaptic(HapticPattern::Double);
  } else if (std::strcmp(pattern, "recording_start") == 0) {
    RequestHaptic(HapticPattern::RecordingStart);
  } else if (std::strcmp(pattern, "long") == 0) {
    RequestHaptic(HapticPattern::Long);
  }
}

void BridgeController::RequestHaptic(HapticPattern pattern, bool onlyIfIdle, bool fromVoiceResult) {
  if (!motorController_ || pattern == HapticPattern::None) return;

  const uint32_t nowMs = NowMs();
  bool start = false;
  taskENTER_CRITICAL();
  if (onlyIfIdle && pendingHaptic_ != HapticPattern::None) {
    // another pattern is waiting
  } else if (pattern == lastHaptic_ && fromVoiceResult != lastHapticFromVoice_ &&
             static_cast<uint32_t>(nowMs - lastHapticMs_) < kHapticDedupMs) {
    // the voice result and the phone's HAPTIC_FEEDBACK are one buzz: the other already asked
    lastHaptic_ = HapticPattern::None;
  } else {
    lastHaptic_ = pattern;
    lastHapticMs_ = nowMs;
    lastHapticFromVoice_ = fromVoiceResult;
    hapticPhase_ = 0;
    pendingHaptic_ = pattern;
    start = true;
  }
  taskEXIT_CRITICAL();

  // Fire the player at once. A pattern still waiting for its second pulse is replaced.
  if (start && !ArmHapticTimer(1)) {
    DropHaptic();
  }
}

// Arm the player's timer. False when there is no timer or the timer daemon's queue was full.
bool BridgeController::ArmHapticTimer(TickType_t ticks) {
  return hapticTimer_ != nullptr && xTimerChangePeriod(hapticTimer_, ticks, 0) == pdPASS;
}

// The player could not be started: forget the pattern, so a waiting one cannot block the
// onlyIfIdle requests (tile result, calendar, find-watch) for good.
void BridgeController::DropHaptic() {
  taskENTER_CRITICAL();
  pendingHaptic_ = HapticPattern::None;
  taskEXIT_CRITICAL();
}

void BridgeController::HapticTimerCb(TimerHandle_t timer) {
  static_cast<BridgeController*>(pvTimerGetTimerID(timer))->StepHaptic();
}

void BridgeController::StepHaptic() {
  uint8_t pulseMs = 0;
  bool second = false;  // the pattern has a second pulse to come

  taskENTER_CRITICAL();
  const HapticPattern current = pendingHaptic_;
  const bool firstPulse = hapticPhase_ == 0;
  switch (current) {
    case HapticPattern::Short:
      pulseMs = 35;
      pendingHaptic_ = HapticPattern::None;
      break;
    case HapticPattern::Long:
      pulseMs = 100;
      pendingHaptic_ = HapticPattern::None;
      break;
    case HapticPattern::Double:
    case HapticPattern::RecordingStart:
    case HapticPattern::FailureDouble:
      // REQ-019 — FailureDouble mirrors Double with longer pulses, to tell "send failed" apart.
      pulseMs = current == HapticPattern::Double ? 25 : current == HapticPattern::RecordingStart ? 20 : 40;
      if (firstPulse) {
        hapticPhase_ = 1;
        second = true;
      } else {
        pendingHaptic_ = HapticPattern::None;
      }
      break;
    default:
      pendingHaptic_ = HapticPattern::None;
      break;
  }
  taskEXIT_CRITICAL();

  if (pulseMs != 0 && motorController_) {
    motorController_->RunForDuration(pulseMs);
  }
  if (second && !ArmHapticTimer(pdMS_TO_TICKS(kHapticGapMs))) {
    DropHaptic();
  }
}

bool BridgeController::SendSingleFrameImpl(MessageType type, const uint8_t* payload, size_t payloadLen, uint8_t* frame, size_t frameSize) {
  SetBreadcrumb(Breadcrumb::BLE_TX, static_cast<uint8_t>(type));
  static constexpr size_t kFrameHeaderLen = 9;

  size_t frameLen = kFrameHeaderLen + payloadLen;
  if (frameLen > frameSize) return false;

  // REQ-012 — reject if the framed payload exceeds the negotiated ATT MTU.
  // Older behaviour silently truncated upstream (Android side, REQ-031) or
  // sent over-MTU frames that the BLE stack dropped. Local fallback: when
  // lastKnownMtu_ is 0 (pre-negotiation), use 23 (ATT min) as the conservative
  // floor so we never send something we know the link can't carry.
  const uint16_t effectiveMtu = (lastKnownMtu_ > 0) ? lastKnownMtu_ : 23;
  const size_t maxPayloadOverMtu = (effectiveMtu > 3) ? (effectiveMtu - 3u) : 0u;
  if (frameLen > maxPayloadOverMtu) {
    oversizeNotifyDrops_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  uint16_t msgId = frameCodec_.GetNextMsgId();

  frame[0] = 0x01;  // version
  frame[1] = static_cast<uint8_t>(type);
  frame[2] = 0x00;  // flags
  frame[3] = static_cast<uint8_t>(msgId & 0xFF);
  frame[4] = static_cast<uint8_t>((msgId >> 8) & 0xFF);
  frame[5] = static_cast<uint8_t>(payloadLen & 0xFF);
  frame[6] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);
  frame[7] = 0;     // chunkIndex
  frame[8] = 1;     // chunkCount

  if (payloadLen > 0 && payload != nullptr) {
    std::memcpy(&frame[kFrameHeaderLen], payload, payloadLen);
  }

  return bleService_.SendNotify(frame, static_cast<uint16_t>(frameLen));
}

// BLE-task frame buffer — used by handlers called from NimBLE host task:
// HandleHello, HandleConfigPush, HandleNotify, HandlePing, HandleWatchFaceLayout (ACK)
bool BridgeController::SendFromBle(MessageType type, const uint8_t* payload, size_t payloadLen) {
  static uint8_t frame[256];
  return SendSingleFrameImpl(type, payload, payloadLen, frame, sizeof(frame));
}

// Display-task frame buffer — used by handlers called from display task:
// OnActionInvoked, SendSensorReport, SendNotifyDismiss
bool BridgeController::SendFromUi(MessageType type, const uint8_t* payload, size_t payloadLen) {
  static uint8_t frame[256];
  return SendSingleFrameImpl(type, payload, payloadLen, frame, sizeof(frame));
}

void BridgeController::SendConfigAck(bool ok, int configVersion) {
  // CBOR encode into STATIC buffer, then send via SendSingleFrame (zero heap allocation).
  // Static for same stack-depth reason as SendSingleFrame.
  static uint8_t buffer[64];
  CborEncoder encoder, mapEncoder;
  cbor_encoder_init(&encoder, buffer, sizeof(buffer), 0);
  cbor_encoder_create_map(&encoder, &mapEncoder, 2);
  cbor_encode_text_stringz(&mapEncoder, "ok");
  cbor_encode_boolean(&mapEncoder, ok);
  cbor_encode_text_stringz(&mapEncoder, "appliedVersion");
  cbor_encode_int(&mapEncoder, configVersion);
  cbor_encoder_close_container(&encoder, &mapEncoder);
  size_t len = cbor_encoder_get_buffer_size(&encoder, buffer);

  SendFromBle(MessageType::CONFIG_ACK, buffer, len);
}

void BridgeController::SendNotifyAck(uint16_t msgId, const char* tag) {
  // CBOR encode into STATIC buffer, then send via SendSingleFrame (zero heap allocation).
  static uint8_t buffer[64];
  CborEncoder encoder, mapEncoder;
  cbor_encoder_init(&encoder, buffer, sizeof(buffer), 0);
  bool hasTag = (tag != nullptr && tag[0] != '\0');
  int mapSize = hasTag ? 3 : 2;
  cbor_encoder_create_map(&encoder, &mapEncoder, mapSize);
  cbor_encode_text_stringz(&mapEncoder, "msgId");
  cbor_encode_int(&mapEncoder, msgId);
  cbor_encode_text_stringz(&mapEncoder, "displayed");
  cbor_encode_boolean(&mapEncoder, true);
  if (hasTag) {
    cbor_encode_text_stringz(&mapEncoder, "tag");
    cbor_encode_text_stringz(&mapEncoder, tag);
  }
  cbor_encoder_close_container(&encoder, &mapEncoder);
  size_t len = cbor_encoder_get_buffer_size(&encoder, buffer);

  SendFromBle(MessageType::NOTIFY_ACK, buffer, len);
}

void BridgeController::SendNotifyDismiss(uint16_t nid) {
  // Display task (SendFromUi's frame buffer); the buffer here is static for the same stack-depth reason.
  static uint8_t buffer[16];
  const size_t len = EncodeNotifyDismiss(nid, buffer, sizeof(buffer));
  if (len == 0) return;

  // Nothing to retry: when the link is down the phone still has the notification, and a dismiss
  // that did not arrive leaves it there, as it was before this message existed.
  SendFromUi(MessageType::NOTIFY_DISMISS, buffer, len);
}

bool BridgeController::EmitWatchFaceAck(bool ok, int version, const char* reason) {
  static uint8_t buffer[96];
  CborEncoder encoder, mapEncoder;
  cbor_encoder_init(&encoder, buffer, sizeof(buffer), 0);
  bool hasReason = (reason != nullptr && reason[0] != '\0');
  int mapSize = hasReason ? 3 : 2;
  cbor_encoder_create_map(&encoder, &mapEncoder, mapSize);
  cbor_encode_text_stringz(&mapEncoder, "ok");
  cbor_encode_boolean(&mapEncoder, ok);
  cbor_encode_text_stringz(&mapEncoder, "appliedVersion");
  cbor_encode_int(&mapEncoder, version);
  if (hasReason) {
    cbor_encode_text_stringz(&mapEncoder, "reason");
    cbor_encode_text_stringz(&mapEncoder, reason);
  }
  cbor_encoder_close_container(&encoder, &mapEncoder);
  size_t len = cbor_encoder_get_buffer_size(&encoder, buffer);

  return SendFromBle(MessageType::WATCHFACE_ACK, buffer, len);
}

void BridgeController::SendWatchFaceAck(bool ok, int version, const char* reason) {
  if (EmitWatchFaceAck(ok, version, reason)) {
    pendingWfAck_.pending = false;
    return;
  }
  // Queue it. HandleWatchFaceLayout runs inside the NimBLE GATT write callback —
  // HA_BLE_RX_MAILBOX is 0, so there is no bridge task — and a notify re-entered
  // from that callback after the ~2 KB layout decode is refused, while the small
  // acks from fast handlers on the same path go out fine. The layout itself still
  // applies, so the watch face visibly updates and the phone times out after 15 s
  // having seen nothing at all.
  pendingWfAck_.ok       = ok;
  pendingWfAck_.version  = version;
  pendingWfAck_.reason   = reason;  // always a literal at the call sites
  pendingWfAck_.attempts = 0;
  pendingWfAck_.pending  = true;
}

void BridgeController::FlushPendingWatchFaceAck() {
  if (!pendingWfAck_.pending) {
    return;
  }
  if (pendingWfAck_.attempts >= kMaxWatchFaceAckAttempts) {
    pendingWfAck_.pending = false;   // give up rather than retry forever
    return;
  }
  pendingWfAck_.attempts++;
  if (EmitWatchFaceAck(pendingWfAck_.ok, pendingWfAck_.version, pendingWfAck_.reason)) {
    pendingWfAck_.pending = false;
  }
}

void BridgeController::OnActionInvoked(uint16_t actionId, const char* gesture) {
  if (motorController_) {
    motorController_->RunForDuration(25);
  }

  // REQ-068 — record which tile triggered this invoke so HandleActionResult can
  // overlay the result glyph on the correct tile.  The display task sets
  // pendingActionTileIndex_ in HomeAssistantService just before calling InvokeAction.
  pendingActionId_        = actionId;
  pendingActionTileIndex_ = bleService_.GetPendingActionTileIndex();
  // Signal "pending" (⌛) immediately so the user sees feedback before the reply arrives.
  if (pendingActionTileIndex_ != 0xFF) {
    bleService_.SetActionResult(1 /*pending*/, pendingActionTileIndex_);
  }

  // Copy entityId into a local buffer BEFORE clearing the pending state.
  // GetPendingTileEntity() returns a pointer to an internal array;
  // ClearPendingTileEntity() zeroes that array.  Without copying first,
  // the CBOR encoder would read the zeroed buffer and encode an empty string,
  // causing Android to skip the toggle (entityId.isNullOrEmpty() == true).
  char entityIdBuf[64] = {0};
  const char* entityId = nullptr;
  // 0xFFFD a watch-face tile, 0xFFFC a home-control tile (HaHomeControl): both name the entity.
  // Until 2.4.89 only 0xFFFD carried it, so the phone dropped every home-control tap.
  if (actionId == 0xFFFD || actionId == 0xFFFC) {
    const char* pending = bleService_.GetPendingTileEntity();
    if (pending && pending[0] != '\0') {
      std::strncpy(entityIdBuf, pending, sizeof(entityIdBuf) - 1);
      entityId = entityIdBuf;
    }
    bleService_.ClearPendingTileEntity();
  }

  const bool sent = SendActionInvoke(actionId, gesture, entityId);

  // REQ-019 — diagnostic double-buzz on send failure, played by the haptic player rather than
  // inline, so the UI handler is not blocked.
  if (!sent) {
    RequestHaptic(HapticPattern::FailureDouble);
  }
}

bool BridgeController::SendActionInvoke(uint16_t actionId, const char* gesture, const char* entityId) {
  // Inline CBOR encode into static buffer — ZERO heap allocation.
  // WHY: After 1+ hours of heap fragmentation from periodic STATE_UPDATE messages,
  // the old code's ActionInvokeMessage (std::string) + EncodeCbor (std::vector)
  // would fail to allocate → operator new → HardFault → freeze on tile tap.
  static uint8_t cborBuf[128];
  CborEncoder encoder, mapEncoder;
  cbor_encoder_init(&encoder, cborBuf, sizeof(cborBuf), 0);

  int mapSize = (entityId != nullptr) ? 3 : 2;
  cbor_encoder_create_map(&encoder, &mapEncoder, mapSize);
  cbor_encode_text_stringz(&mapEncoder, "actionId");
  cbor_encode_uint(&mapEncoder, actionId);
  cbor_encode_text_stringz(&mapEncoder, "gesture");
  cbor_encode_text_stringz(&mapEncoder, gesture);
  if (entityId != nullptr) {
    cbor_encode_text_stringz(&mapEncoder, "entityId");
    cbor_encode_text_stringz(&mapEncoder, entityId);
  }
  cbor_encoder_close_container(&encoder, &mapEncoder);

  size_t cborLen = cbor_encoder_get_buffer_size(&encoder, cborBuf);
  return SendFromUi(MessageType::ACTION_INVOKE, cborBuf, cborLen);
}

// The voice trigger. No tile is involved: pendingActionId_ and the tile index stay as they are.
// HandleActionResult takes every 0xFFFE result before its stale check, session or not, and
// never touches a tile glyph for it.
bool BridgeController::OnButtonDoubleClicked() {
  // The service's connection handle is set at the GAP connect of ANY central (NimbleController)
  // and cleared on disconnect, so a non-zero MTU says only that some device is connected.
  // voiceEnabled_ says the app is, and has voice on: it comes from its CONFIG_PUSH.
  if (bleService_.GetCurrentMtu() == 0 || !voiceEnabled_.load(std::memory_order_acquire)) {
    return false;
  }
  // "double" cancels a running session; the session ends when the phone says so.
  const bool cancel = VoiceSessionActive();
  if (!cancel) {
    // Active before the send, so a result that races back finds the session. Provisional until
    // the first result (kVoiceStartTimeoutMs).
    MarkVoiceSession(true, false);
  }
  if (!SendActionInvoke(VOICE_ASSIST_ACTION_ID, cancel ? "double" : "tap", nullptr)) {
    if (!cancel) {
      MarkVoiceSession(false, false);
    }
    RequestHaptic(HapticPattern::FailureDouble);
  }
  return true;
}

bool BridgeController::VoiceSessionActive() {
  constexpr uint32_t kStampMask = 0x3FFFFFFFu;
  uint32_t word = voiceState_.load(std::memory_order_acquire);
  while ((word & 1u) != 0) {
    const uint32_t elapsed = (NowMs() - (word >> 2)) & kStampMask;
    const uint32_t limit = (word & 2u) != 0 ? kVoiceSessionTimeoutMs : kVoiceStartTimeoutMs;
    if (elapsed < limit) {
      return true;
    }
    // Timed out: clear it, unless a result changed the word since it was read.
    if (voiceState_.compare_exchange_weak(word, 0, std::memory_order_acq_rel, std::memory_order_acquire)) {
      return false;
    }
  }
  return false;
}

void BridgeController::MarkVoiceSession(bool active, bool resultSeen) {
  if (!active) {
    voiceState_.store(0, std::memory_order_release);
    return;
  }
  voiceState_.store(((NowMs() & 0x3FFFFFFFu) << 2) | (resultSeen ? 2u : 0u) | 1u, std::memory_order_release);
}

void BridgeController::SendSensorReport() {
  SetBreadcrumb(Breadcrumb::SENSOR_REPORT);

  // Inline CBOR encode into STATIC buffer — ZERO heap allocation.
  // WHY: The old code created SensorReportMessage then called EncodeCbor which output
  // to std::vector<uint8_t>. Every 60s this allocated+freed ~100 bytes on the heap.
  // Over hours of operation, this contributed to heap fragmentation.
  static uint8_t buffer[256];
  static_assert(kSensorReportMaxBytes <= sizeof(buffer), "the buffer holds the largest SENSOR_REPORT");
  size_t len = 0;
  {
    // The map and its keys: ha_bridge/SensorReport.h, which the host tests encode too.
    SensorReport report;
    report.steps = sensorCollector_.GetSteps();
    report.heartRate = sensorCollector_.GetHeartRate();
    report.heartRateState = sensorCollector_.GetHeartRateState();
    report.batteryPercent = sensorCollector_.GetBatteryPercent();
    report.batteryCharging = sensorCollector_.IsBatteryCharging();
    report.batteryMv = sensorCollector_.GetBatteryMillivolts();
    report.powerPresent = sensorCollector_.IsPowerPresent();
    sensorCollector_.GetMotion(report.motionX, report.motionY, report.motionZ);
    report.timestamp = sensorCollector_.GetTimestamp();
    report.freeHeap = xPortGetFreeHeapSize();
    report.minFreeHeap = xPortGetMinimumEverFreeHeapSize();
    report.stackHighWater = uxTaskGetStackHighWaterMark(nullptr);

    // REQ-056 — LVGL diagnostic fields (hello_diag_rev=2).
    // Snapshot once so the three derived values are consistent.
    lv_mem_monitor_t lvMon = {};
    lv_mem_monitor(&lvMon);
    report.lvFreeSize = static_cast<uint32_t>(lvMon.free_size);
    report.lvFreeBiggest = static_cast<uint32_t>(lvMon.free_biggest_size);
    report.lvFragPct = (report.lvFreeSize > 0)
        ? static_cast<uint8_t>(((report.lvFreeSize - report.lvFreeBiggest) * 100u) / report.lvFreeSize)
        : 0u;
    report.lvCreateFailures = bleService_.GetLvCreateFailures();

    // DASHBOARD_STATE health (dsOk, dsBad, fpMis, dsAge).
    report.dashStateOk = bleService_.GetDashStateOk();
    report.dashStateRefused = bleService_.GetDashStateRefused();
    report.fingerprintMisses = bleService_.GetFingerprintMisses();
    report.dashStateAgeSec = bleService_.SecondsSinceDashState();

    len = EncodeSensorReport(report, buffer, sizeof(buffer));
  }
  if (len == 0) return;

  SendFromUi(MessageType::SENSOR_REPORT, buffer, len);
}

void BridgeController::OnTick() {
  currentTick_++;

  FlushPendingWatchFaceAck();   // display-task context, not the BLE callback
  ProcessFindWatch();           // REQ-072 — vibrate periodically while find-watch alert is active
  ProcessCalendarVibration();   // REQ-070 — one-shot vibration at calendar event start

  // Poll for MTU renegotiation every ~10 s (kMtuPollIntervalTicks)
  static uint8_t mtuPollCounter = 0;
  if (++mtuPollCounter >= kMtuPollIntervalTicks) {
    mtuPollCounter = 0;

    uint16_t currentMtu = bleService_.GetCurrentMtu();
    if (currentMtu != lastKnownMtu_ && currentMtu > 0) {
      SetMtu(currentMtu);
    }
  }

  if (!infinassistantEnabled_) {
    return;
  }

  // Sensor report interval: configurable via CONFIG_PUSH (default 60 s),
  // measured in wall-clock ms so an irregular OnTick cadence cannot stretch it.
  const uint32_t nowMs = NowMs();
  if (lastSensorReportMs_ == 0) lastSensorReportMs_ = nowMs;
  if (static_cast<uint32_t>(nowMs - lastSensorReportMs_) >= sensorReportIntervalMs_) {
    SendSensorReport();
    lastSensorReportMs_ = nowMs;
    // REQ-060: sensor report path is alive — kick the Sensor subsystem heartbeat.
#if HA_WATCHDOG_SUBSYSTEM_AWARE
    WatchdogSubsystemKick(WatchdogSubsystem::Sensor);
#endif
  }

#if HA_WATCHDOG_SUBSYSTEM_AWARE
  // REQ-060 — subsystem-aware watchdog gate.
  //
  // OnTick runs every 20ms (LV_DISP_DEF_REFR_PERIOD) from the display task.
  // The System subsystem heartbeat is unconditionally set here — if OnTick is
  // running the display task is alive.  The BLE subsystem heartbeat is set by
  // BleProcessingTaskLoop after each drain cycle.  The Display heartbeat is set
  // by HomeAssistant::Refresh.  The Sensor heartbeat is set above when a sensor
  // report fires.
  //
  // Watchdog kick gate: every kWatchdogCheckIntervalTicks (≈ 500ms = 25 ticks
  // at kTickPeriodMs per tick), check whether all subsystems beat.  If yes: reload the
  // watchdog and clear all flags so the next window opens fresh.  If no: record
  // the missing-mask in g_crashContext and do NOT reload — the hardware watchdog
  // will fire and reboot within its timeout, with forensic data intact.
  WatchdogSubsystemKick(WatchdogSubsystem::System);

  static constexpr uint32_t kWatchdogCheckIntervalTicks = 1;  // ~1 s
  if ((currentTick_ % kWatchdogCheckIntervalTicks) == 0 && watchdog_) {
    const uint8_t missingMask = WatchdogBuildMissingMask();
    if (missingMask == 0) {
      // All subsystems alive — kick the watchdog and reset flags for next window.
      watchdog_->Reload();
      for (uint8_t i = 0; i < kWatchdogSubsystemCount; ++i) {
        g_watchdogHeartbeat[i] = 0;
      }
    } else {
      // One or more subsystems silent — record for post-mortem and let the WDT fire.
#if HA_CRASH_CONTEXT_EXTENDED
      g_crashContext.watchdogMask = missingMask;
      g_crashContext.magic        = kCrashContextMagic; // arm the context for next boot
#endif
      // Do NOT reload: hardware WDT will fire within its timeout period.
    }
  }

#else // !HA_WATCHDOG_SUBSYSTEM_AWARE — rollback to single-source kick

  // Original central-kick path (restores 2.4.10 behavior when flag is off).
  static constexpr uint32_t kLegacyKickIntervalTicks = 1;  // ~1 s
  if ((currentTick_ % kLegacyKickIntervalTicks) == 0 && watchdog_) {
    watchdog_->Reload();
  }

#endif // HA_WATCHDOG_SUBSYSTEM_AWARE

}

namespace {

const char* LayoutRejectReason(LayoutVerdict verdict) {
  switch (verdict) {
    case LayoutVerdict::ValidationError: return "validation_error";
    case LayoutVerdict::NoTarget:        return "oom";
    default:                             return "decode_error";
  }
}

// The ACK reason the app keys its one bounded retry on
// (WatchFaceOrchestrator.PENDING_PUSH_REASON).
constexpr const char* kPendingPushReason = "pending";

// The reason an ok=true ACK carries when the layout held more screens than the
// watch takes (LayoutService::kMaxScreens) and the surplus was skipped. The app
// reads `reason` of an ok ACK as optional text and ignores it.
constexpr const char* kScreensTrimmedReason = "screens_trimmed";

}  // namespace

// Runs inside the NimBLE GATT write callback: validate, hand off, ACK. It
// allocates nothing, calls no LVGL function and waits for nothing (NFR-4): the
// validate pass has no heap and no store access, the hand-off is pointer stores
// and one atomic byte, and arming the step timer is a zero-tick queue post. The
// write itself (LayoutWriter::Step) runs later on the timer daemon.
void BridgeController::HandleWatchFaceLayout(const uint8_t* payload, size_t len, uint16_t msgId) {
  SetBreadcrumb(Breadcrumb::WATCHFACE_APPLY, static_cast<uint8_t>(MessageType::WATCHFACE_LAYOUT), msgId);

  const LayoutDecodeResult result = LayoutWriter::Validate(payload, len);
  if (result.verdict != LayoutVerdict::Ok) {
    // Nothing was written and nothing changed. The buffer is still the codec's:
    // it frees it and gives the heap phase back once this handler returns.
    SendWatchFaceAck(false, result.version, LayoutRejectReason(result.verdict));
    return;
  }

#if HA_CRASH_CONTEXT_EXTENDED
  // Taken before the hand-off: the writer owns (and frees) the payload after it.
  const uint32_t payloadCrc = Crc32Nibble(payload, len);
#endif

  LayoutPipeline& pipeline = *layout_;
  pipeline.store.Bind(bleService_.FileSystem());
  if (!pipeline.store.Bound() || pipeline.timer == nullptr) {
    RefuseLayoutHandOff(result.version);
    return;
  }

  // Take the reassembly buffer from the codec: from here the writer owns it and
  // frees it exactly once, when the write ends.
  uint8_t* buf = frameCodec_.TakeBuffer(payload);
  if (buf == nullptr) {
    RefuseLayoutHandOff(result.version);
    return;
  }
  if (!pipeline.writer.HandOff(buf, len)) {
    vPortFree(buf);
    pipeline.writer.AbortTransfer();
    RefuseLayoutHandOff(result.version);
    return;
  }
  // The timer has been the stall watchdog of this transfer; from here it paces
  // the write. ChangePeriod restarts it with the one-tick step period (a plain
  // xTimerStart would keep the stall period). Until the daemon applies this, an
  // expiry still reads the stall period and takes no step, so CancelHandOff below
  // is valid: no step has run.
  if (xTimerChangePeriod(pipeline.timer, kStepPeriodTicks, 0) != pdPASS) {
    pipeline.writer.CancelHandOff();
    RefuseLayoutHandOff(result.version);
    return;
  }

#if HA_CRASH_CONTEXT_EXTENDED
  // Record the layout fingerprint at hand-off. The first screen's type is not
  // decoded on this path, so it is recorded as 0.
  UpdateCrashContextLayout(0u, bleService_.GetLayoutGen(), payloadCrc);
#endif // HA_CRASH_CONTEXT_EXTENDED

  // The app answers the ACK with the new tiles' states: they replace the old layout's
  // in the entity table (one atomic store here; the table is emptied by the first of them).
  bleService_.ResetEntitiesOnNextUpdate();

  // The ACK reports the hand-off. The record is durable, and the update signal
  // raised, when the writer's last step commits (RunLayoutStep).
  SendWatchFaceAck(true, result.version, result.screensTrimmed ? kScreensTrimmedReason : nullptr);
}

void BridgeController::RefuseLayoutHandOff(int version) {
  ++layoutHandOffRefused_;
  // The transfer is over (the buffer went back, or the codec still holds it and
  // ends it): the display gets its window before the next one is admitted.
  frameCodec_.NoteTransferEnded(CodecClockMs());
  SendWatchFaceAck(false, version, LayoutRejectReason(LayoutVerdict::NoTarget));
}

void BridgeController::OnLayoutTransferRefused(uint16_t msgId) {
  (void)msgId;
  SendWatchFaceAck(false, 0, kPendingPushReason);
}

void BridgeController::OnLayoutTransferStalled() {
  frameCodec_.ExpireStalled(CodecClockMs());
  // A queued refusal notice is retried here too: this handler runs on the host
  // task outside any GATT write callback, the state in which a notify goes out.
  FlushPendingWatchFaceAck();
}

// Clear() frees the reassembly buffer and ends the gate only for a transfer the
// codec still owns. After TakeBuffer() the buffer belongs to the writer and the
// gate is no longer held, so this is a no-op for a pending write.
void BridgeController::OnBleDisconnected() {
  if (g_layoutStepOwner != nullptr) {
    g_layoutStepOwner->frameCodec_.Clear(CodecClockMs());
    // Whatever connects next must say again that it has voice on (its CONFIG_PUSH), and no
    // session survives the link.
    g_layoutStepOwner->voiceEnabled_.store(false, std::memory_order_release);
    g_layoutStepOwner->voiceState_.store(0, std::memory_order_release);
  }
}

// One step, then whatever follows it. The only frame that stays on the daemon
// stack across a step is this one: everything that needs locals (the re-arm, the
// publish) is in FinishLayoutStep, called after Step() has returned.
void BridgeController::RunLayoutStep() {
  // The layout timer's other role: a period other than the step period means the
  // timer expired as the stall watchdog of a transfer still reassembling (or one
  // that just handed off, before its ChangePeriod is applied): no write step is
  // due. The daemon applies timer commands and runs callbacks on this one task,
  // so the period read here is the one in effect for this expiry.
  if (xTimerGetPeriod(layout_->timer) != kStepPeriodTicks) {
    layout_->PostStall();
    return;
  }
  if (!LayoutWakeReady()) return;
  while (FinishLayoutStep(layout_->writer.Step())) {
  }
}

// The write touches the flash, so it runs under a system wake lock and takes no
// step until the system is running: a sleeping watch has the SPI bus off (and,
// in always-on display, the flash powered down), and a store call then spins
// forever on the bus and kills every software timer.
//
// After the request is posted, Running means the bus is on and stays on:
// GoToSleep() returns early while a wake lock is held, and a sleep already
// under way is undone by the request itself. The request is processed in queue
// order and calls GoToRunning, which wakes the SPI and the flash.
//
// The lock is asked for once per write and given back when the write ends, or
// when it is dropped here. Abandon only succeeds before the first step, so a
// write that has started is never dropped half-way. A push that arrives while
// the watch sleeps therefore also turns the screen on (GoToRunning); that is
// accepted, and the face then redraws with the new layout.
//
// True when a step may run now. False: the timer has been re-armed to ask
// again in a tick, or the write was dropped.
bool BridgeController::LayoutWakeReady() {
  LayoutPipeline& p = *layout_;
  // Step() makes no store call when nothing is pending.
  if (!p.writer.Busy()) return true;
  if (!p.wakeHeld) {
    bleService_.RequestWake();
    p.wakeHeld = true;
    p.wakeAskedAt = xTaskGetTickCount();
  }
  if (bleService_.SystemAwake()) return true;
  if ((xTaskGetTickCount() - p.wakeAskedAt) >= kWakeWaitTicks && p.writer.Abandon()) {
    ReleaseLayoutWake();
    frameCodec_.NoteTransferEnded(CodecClockMs());
    return false;
  }
  // Ask again in a tick. If that retry cannot be queued, drop the write rather
  // than strand it (it has not started, so Abandon succeeds).
  if (xTimerStart(p.timer, 0) != pdPASS && p.writer.Abandon()) {
    ReleaseLayoutWake();
    frameCodec_.NoteTransferEnded(CodecClockMs());
  }
  return false;
}

void BridgeController::ReleaseLayoutWake() {
  if (layout_->wakeHeld) {
    layout_->wakeHeld = false;
    bleService_.ReleaseWake();
  }
}

// After a step. Continue: re-arm the one-tick timer; when that cannot be queued
// (the daemon's queue is full) return true and the next step runs inline, since
// the write is never abandoned: an abandoned write would strand an open file
// handle and the retained buffer. Committed: publish.
bool BridgeController::FinishLayoutStep(StepResult result) {
  switch (result) {
    case StepResult::Continue:
      return xTimerStart(layout_->timer, 0) != pdPASS;

    case StepResult::Committed:
      // The write is over and the phase is back: start the cooldown that keeps
      // the phone from taking the phase again before the display has had a turn.
      frameCodec_.NoteTransferEnded(CodecClockMs());
      // Publish order: data (the directory commit, already durable), the
      // generation, then the update signal.
      __DMB();
      bleService_.PublishLayoutCommitted();
      ReleaseLayoutWake();
      return false;

    case StepResult::Failed:  // the previous record stands and nothing is published
      frameCodec_.NoteTransferEnded(CodecClockMs());
      ReleaseLayoutWake();
      return false;

    default:  // Idle: nothing was pending
      return false;
  }
}

uint32_t BridgeController::LayoutRefusedCount() const {
  return frameCodec_.refused() + frameCodec_.cooldownRefused() + layout_->writer.refused() + layout_->notReadyRefused +
         layoutHandOffRefused_;
}

uint32_t BridgeController::LayoutStallArmFailedCount() const {
  return layout_->stallArmFailed;
}

uint32_t BridgeController::LayoutCommittedCount() const {
  return layout_->writer.committed();
}

uint32_t BridgeController::LayoutFailedCount() const {
  return layout_->writer.failed();
}

void BridgeController::HandleTimeSync(const uint8_t* payload, size_t len) {
  // Inline zero-heap CBOR decode — replaces TimeSyncMessage::DecodeCbor
  // which used std::string internally. Only needs timestamp + timezoneOffset.
  CborParser parser;
  CborValue root, map;
  if (cbor_parser_init(payload, len, 0, &parser, &root) != CborNoError) return;
  if (!cbor_value_is_map(&root)) return;
  if (cbor_value_enter_container(&root, &map) != CborNoError) return;

  int64_t timestamp = 0;
  int32_t timezoneOffset = 0;

  while (!cbor_value_at_end(&map)) {
    if (!cbor_value_is_text_string(&map)) {
      if (!SkipUnknownEntry(&map)) return;
      continue;
    }
    bool match = false;

    cbor_value_text_string_equals(&map, "timestamp", &match);
    if (match) {
      cbor_value_advance(&map);
      if (!cbor_value_at_end(&map) && cbor_value_is_integer(&map)) {
        cbor_value_get_int64(&map, &timestamp);
      }
      if (!cbor_value_at_end(&map) && cbor_value_advance(&map) != CborNoError) return;
      continue;
    }

    cbor_value_text_string_equals(&map, "timezoneOffset", &match);
    if (match) {
      cbor_value_advance(&map);
      if (!cbor_value_at_end(&map) && cbor_value_is_integer(&map)) {
        int64_t v = 0;
        cbor_value_get_int64(&map, &v);
        timezoneOffset = static_cast<int32_t>(v);
      }
      if (!cbor_value_at_end(&map) && cbor_value_advance(&map) != CborNoError) return;
      continue;
    }

    if (!SkipUnknownEntry(&map)) return;
  }

  static constexpr int64_t MIN_VALID_TIMESTAMP = 946684800;
  static constexpr int64_t MAX_VALID_TIMESTAMP = 4102444800;

  if (timestamp < MIN_VALID_TIMESTAMP || timestamp > MAX_VALID_TIMESTAMP) {
    return;
  }

  // Helper: decompose a UTC epoch into calendar fields and call SetTime.
  auto applyTimestamp = [this](int64_t localTimestamp) {
    const int64_t SECONDS_PER_DAY = 86400;
    const int64_t SECONDS_PER_HOUR = 3600;
    const int64_t SECONDS_PER_MINUTE = 60;

    int64_t daysSinceEpoch = localTimestamp / SECONDS_PER_DAY;
    int64_t secondsToday = localTimestamp % SECONDS_PER_DAY;
    if (secondsToday < 0) {
      secondsToday += SECONDS_PER_DAY;
      daysSinceEpoch -= 1;
    }

    uint8_t hours = (secondsToday / SECONDS_PER_HOUR) % 24;
    uint8_t minutes = (secondsToday % SECONDS_PER_HOUR) / SECONDS_PER_MINUTE;
    uint8_t seconds = secondsToday % SECONDS_PER_MINUTE;

    int year = 1970;
    int64_t daysRemaining = daysSinceEpoch;

    if (daysRemaining >= 10957) {
      daysRemaining -= 10957;
      year = 2000;
    }

    while (daysRemaining >= 365) {
      bool isLeap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
      int daysInYear = isLeap ? 366 : 365;
      if (daysRemaining >= daysInYear) {
        daysRemaining -= daysInYear;
        year++;
      } else {
        break;
      }
    }

    const int daysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool isLeap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);

    uint8_t month = 1;
    for (int m = 0; m < 12; m++) {
      int days = daysInMonth[m];
      if (m == 1 && isLeap) days = 29;
      if (daysRemaining >= days) {
        daysRemaining -= days;
        month++;
      } else {
        break;
      }
    }

    uint8_t day = static_cast<uint8_t>(daysRemaining + 1);
    dateTimeController_->SetTime(year, month, day, hours, minutes, seconds);
  };

  if (dateTimeController_) {
    int64_t localTimestamp = timestamp + timezoneOffset;

    if (localTimestamp < MIN_VALID_TIMESTAMP || localTimestamp > MAX_VALID_TIMESTAMP) {
      return;
    }

    // Compute current watch UTC epoch from DateTimeController.
    auto nowUtc = dateTimeController_->UTCDateTime();
    int64_t watchEpoch = std::chrono::duration_cast<std::chrono::seconds>(
        nowUtc.time_since_epoch()).count();

    int64_t delta = timestamp - watchEpoch;

    // The clock is set to local wall time; the zone offset is what UTCDateTime() takes
    // off it, in quarters of an hour as the BLE CTS Local Time characteristic gives it.
    // Until 2.4.92 it was never set, so UTCDateTime() returned local time: on a phone
    // at UTC-6 the meeting countdowns, the next-event header and its buzz ran 6 h late,
    // and every sync saw a 6 h delta and set the clock again.
    const int8_t zoneQuarters = static_cast<int8_t>(timezoneOffset / (15 * 60));
    const bool zoneChanged = dateTimeController_->UtcOffset() != zoneQuarters;

    // Apply if the absolute drift exceeds 5 seconds, or the zone changed (DST, travel):
    // then the UTC instant can match while the local time is an hour or more off.
    static constexpr int64_t kMinDeltaSeconds = 5;
    uint8_t status;
    int32_t appliedDelta;

    if (zoneChanged || delta > kMinDeltaSeconds || delta < -kMinDeltaSeconds) {
      applyTimestamp(localTimestamp);
      dateTimeController_->SetTimeZone(zoneQuarters, 0);
      status = 0;  // OK — applied
      appliedDelta = static_cast<int32_t>(delta);
    } else {
      status = 1;  // REJECTED — delta too small
      appliedDelta = 0;
    }

    // Read back the post-adjustment UTC epoch for the ACK.
    auto postUtc = dateTimeController_->UTCDateTime();
    uint32_t watchUtcEpoch = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            postUtc.time_since_epoch()).count());

    // Send TIME_SYNC_ACK: {"s": status, "d": appliedDelta, "t": watchUtcEpoch}
    static uint8_t ackBuf[64];
    CborEncoder encoder, mapEncoder;
    cbor_encoder_init(&encoder, ackBuf, sizeof(ackBuf), 0);
    cbor_encoder_create_map(&encoder, &mapEncoder, 3);
    cbor_encode_text_stringz(&mapEncoder, "s");
    cbor_encode_uint(&mapEncoder, status);
    cbor_encode_text_stringz(&mapEncoder, "d");
    cbor_encode_int(&mapEncoder, appliedDelta);
    cbor_encode_text_stringz(&mapEncoder, "t");
    cbor_encode_uint(&mapEncoder, watchUtcEpoch);
    cbor_encoder_close_container(&encoder, &mapEncoder);
    size_t ackLen = cbor_encoder_get_buffer_size(&encoder, ackBuf);

    SendFromBle(MessageType::TIME_SYNC_ACK, ackBuf, ackLen);
  }
}

void BridgeController::SetMtu(uint16_t mtu) {
  if (mtu < 23) {
    mtu = 23;
  }
  if (mtu > 517) {
    mtu = 517;
  }

  frameCodec_.SetMtu(mtu);
  lastKnownMtu_ = mtu;
}

void BridgeController::HandleLogbookUpdate(const uint8_t* payload, size_t len) {
  if (payload == nullptr || len == 0) return;

  CborParser parser;
  CborValue root;
  if (cbor_parser_init(payload, len, 0, &parser, &root) != CborNoError) return;
  if (!cbor_value_is_map(&root)) return;

  CborValue map;
  if (cbor_value_enter_container(&root, &map) != CborNoError) return;

  // REQ-018 — zero-initialize entries before partial CBOR decode so unset
  // fields hold zero, not stack garbage. Required-field gating below ensures
  // a count++ never publishes an entry without (t, s).
  Pinetime::Controllers::HomeAssistantService::LogbookEntry
    entries[Pinetime::Controllers::HomeAssistantService::kMaxLogbookEntries] = {};
  uint8_t count = 0;

  while (!cbor_value_at_end(&map)) {
    bool match = false;

    cbor_value_text_string_equals(&map, "entries", &match);
    if (match) {
      cbor_value_advance(&map);
      if (!cbor_value_at_end(&map) && cbor_value_is_array(&map)) {
        CborValue arr;
        if (cbor_value_enter_container(&map, &arr) == CborNoError) {
          while (!cbor_value_at_end(&arr) &&
                 count < Pinetime::Controllers::HomeAssistantService::kMaxLogbookEntries) {
            if (!cbor_value_is_map(&arr)) { cbor_value_advance(&arr); continue; }
            CborValue entry;
            if (cbor_value_enter_container(&arr, &entry) != CborNoError) {
              cbor_value_advance(&arr);
              continue;
            }

            bool gotT = false, gotS = false;
            while (!cbor_value_at_end(&entry)) {
              bool ematch = false;
              cbor_value_text_string_equals(&entry, "t", &ematch);
              if (ematch) {
                cbor_value_advance(&entry);
                DecodeStringInto(&entry, entries[count].time.data(), entries[count].time.size());
                gotT = true;
                continue;
              }
              cbor_value_text_string_equals(&entry, "s", &ematch);
              if (ematch) {
                cbor_value_advance(&entry);
                DecodeStringInto(&entry, entries[count].state.data(), entries[count].state.size());
                gotS = true;
                continue;
              }
              cbor_value_text_string_equals(&entry, "n", &ematch);
              if (ematch) {
                cbor_value_advance(&entry);
                DecodeStringInto(&entry, entries[count].subject.data(), entries[count].subject.size());
                continue;
              }
              // Unknown key — skip key + value
              if (!SkipUnknownEntry(&entry)) return;
            }
            cbor_value_leave_container(&arr, &entry);
            // REQ-018 — only publish the slot if required fields (t, s) were
            // both present. Missing required fields leave the slot zero-init.
            if (gotT && gotS) {
              count++;
            } else {
              entries[count] = {};  // discard partial decode
            }
          }
          // Drain remaining items before leaving container
          while (!cbor_value_at_end(&arr)) cbor_value_advance(&arr);
          cbor_value_leave_container(&map, &arr);
        }
      } else if (!cbor_value_at_end(&map)) {
        cbor_value_advance(&map);
      }
      continue;
    }

    // Unknown top-level key — skip key + value
    if (!SkipUnknownEntry(&map)) return;
  }

  bleService_.SetLogbookEntries(entries, count);
}

void BridgeController::HandleCalendarUpdate(const uint8_t* payload, size_t len) {
  if (payload == nullptr || len == 0) return;

  CborParser parser;
  CborValue root;
  if (cbor_parser_init(payload, len, 0, &parser, &root) != CborNoError) return;
  if (!cbor_value_is_map(&root)) return;

  CborValue map;
  if (cbor_value_enter_container(&root, &map) != CborNoError) return;

  // REQ-018 — zero-init before partial decode; required-field gating below.
  Pinetime::Controllers::HomeAssistantService::CalendarAppEntry
    entries[Pinetime::Controllers::HomeAssistantService::kMaxCalendarEntries] = {};
  uint8_t count = 0;
  int8_t dayOffset = 0;

  while (!cbor_value_at_end(&map)) {
    bool match = false;

    cbor_value_text_string_equals(&map, "day", &match);
    if (match) {
      cbor_value_advance(&map);
      if (!cbor_value_at_end(&map) && cbor_value_is_integer(&map)) {
        int64_t v = 0;
        cbor_value_get_int64(&map, &v);
        dayOffset = static_cast<int8_t>(v);
      }
      if (!cbor_value_at_end(&map) && cbor_value_advance(&map) != CborNoError) return;
      continue;
    }

    cbor_value_text_string_equals(&map, "entries", &match);
    if (match) {
      cbor_value_advance(&map);
      if (!cbor_value_at_end(&map) && cbor_value_is_array(&map)) {
        CborValue arr;
        if (cbor_value_enter_container(&map, &arr) == CborNoError) {
          while (!cbor_value_at_end(&arr) &&
                 count < Pinetime::Controllers::HomeAssistantService::kMaxCalendarEntries) {
            if (!cbor_value_is_map(&arr)) { cbor_value_advance(&arr); continue; }
            CborValue entry;
            if (cbor_value_enter_container(&arr, &entry) != CborNoError) {
              cbor_value_advance(&arr);
              continue;
            }
            bool gotT = false, gotN = false;
            while (!cbor_value_at_end(&entry)) {
              bool ematch = false;
              cbor_value_text_string_equals(&entry, "t", &ematch);
              if (ematch) {
                cbor_value_advance(&entry);
                DecodeStringInto(&entry, entries[count].time.data(), entries[count].time.size());
                gotT = true;
                continue;
              }
              cbor_value_text_string_equals(&entry, "n", &ematch);
              if (ematch) {
                cbor_value_advance(&entry);
                DecodeStringInto(&entry, entries[count].title.data(), entries[count].title.size());
                gotN = true;
                continue;
              }
              // Unknown key — skip key + value
              if (!SkipUnknownEntry(&entry)) return;
            }
            cbor_value_leave_container(&arr, &entry);
            // REQ-018 — only publish if required (t, n) both present.
            if (gotT && gotN) {
              count++;
            } else {
              entries[count] = {};
            }
          }
          // Drain remaining items before leaving container
          while (!cbor_value_at_end(&arr)) cbor_value_advance(&arr);
          cbor_value_leave_container(&map, &arr);
        }
      } else if (!cbor_value_at_end(&map)) {
        cbor_value_advance(&map);
      }
      continue;
    }

    // Unknown top-level key — skip key + value
    if (!SkipUnknownEntry(&map)) return;
  }

  bleService_.SetCalendarEntries(entries, count, dayOffset);
}

// The dashboard's live state (2.4.96). Decoded into a staging copy on this task's stack and
// published whole: a malformed or cut-short message leaves the face with the state it had.
// Both outcomes are counted for SENSOR_REPORT: dsBad here, dsOk in PublishDashLive.
void BridgeController::HandleDashboardState(const uint8_t* payload, size_t len) {
  DashLive staging;
  if (!DecodeDashboardState(payload, len, staging)) {
    bleService_.CountDashStateRefused();
    return;
  }
  bleService_.PublishDashLive(staging);
}

void BridgeController::HandleHomeControlUpdate(const uint8_t* payload, size_t len) {
  if (payload == nullptr || len == 0) return;

  CborParser parser;
  CborValue root;
  if (cbor_parser_init(payload, len, 0, &parser, &root) != CborNoError) return;
  if (!cbor_value_is_map(&root)) return;

  CborValue map;
  if (cbor_value_enter_container(&root, &map) != CborNoError) return;

  // REQ-018 — zero-init before partial decode; required-field gating below.
  Pinetime::Controllers::HomeAssistantService::HomeControlDevice
    devices[Pinetime::Controllers::HomeAssistantService::kMaxHomeControlDevices] = {};
  uint8_t count = 0;

  while (!cbor_value_at_end(&map)) {
    bool match = false;

    cbor_value_text_string_equals(&map, "devices", &match);
    if (match) {
      cbor_value_advance(&map);
      if (!cbor_value_at_end(&map) && cbor_value_is_array(&map)) {
        CborValue arr;
        if (cbor_value_enter_container(&map, &arr) == CborNoError) {
          while (!cbor_value_at_end(&arr) &&
                 count < Pinetime::Controllers::HomeAssistantService::kMaxHomeControlDevices) {
            if (!cbor_value_is_map(&arr)) { cbor_value_advance(&arr); continue; }
            CborValue entry;
            if (cbor_value_enter_container(&arr, &entry) != CborNoError) {
              cbor_value_advance(&arr);
              continue;
            }
            bool gotL = false, gotE = false;
            while (!cbor_value_at_end(&entry)) {
              bool ematch = false;
              cbor_value_text_string_equals(&entry, "l", &ematch);
              if (ematch) { cbor_value_advance(&entry); DecodeStringInto(&entry, devices[count].label.data(), devices[count].label.size()); gotL = true; continue; }
              cbor_value_text_string_equals(&entry, "e", &ematch);
              if (ematch) { cbor_value_advance(&entry); DecodeStringInto(&entry, devices[count].entityId.data(), devices[count].entityId.size()); gotE = true; continue; }
              cbor_value_text_string_equals(&entry, "s", &ematch);
              if (ematch) { cbor_value_advance(&entry); DecodeStringInto(&entry, devices[count].state.data(), devices[count].state.size()); continue; }
              cbor_value_text_string_equals(&entry, "a", &ematch);
              if (ematch) {
                cbor_value_advance(&entry);
                if (!cbor_value_at_end(&entry) && cbor_value_is_boolean(&entry)) {
                  bool val = false;
                  cbor_value_get_boolean(&entry, &val);
                  devices[count].active = val;
                }
                if (!cbor_value_at_end(&entry) && cbor_value_advance(&entry) != CborNoError) return;
                continue;
              }
              // Unknown key — skip key + value
              if (!SkipUnknownEntry(&entry)) return;
            }
            cbor_value_leave_container(&arr, &entry);
            // REQ-018 — only publish if required (l, e) both present.
            if (gotL && gotE) {
              count++;
            } else {
              devices[count] = {};
            }
          }
          // Drain remaining items before leaving container
          while (!cbor_value_at_end(&arr)) cbor_value_advance(&arr);
          cbor_value_leave_container(&map, &arr);
        }
      } else if (!cbor_value_at_end(&map)) {
        cbor_value_advance(&map);
      }
      continue;
    }

    // Unknown top-level key — skip key + value
    if (!SkipUnknownEntry(&map)) return;
  }

  bleService_.SetHomeControlDevices(devices, count);
}

void BridgeController::SendCalendarNav(int8_t dayOffset) {
  static uint8_t buf[32];
  CborEncoder encoder, mapEnc;
  cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
  if (cbor_encoder_create_map(&encoder, &mapEnc, 1) != CborNoError) return;
  if (cbor_encode_text_stringz(&mapEnc, "day") != CborNoError) return;
  if (cbor_encode_int(&mapEnc, dayOffset) != CborNoError) return;
  if (cbor_encoder_close_container(&encoder, &mapEnc) != CborNoError) return;
  size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
  SendFromUi(MessageType::CALENDAR_NAV, buf, len);
}

// ---------------------------------------------------------------------------
// REQ-072 — FIND_WATCH handler.
//
// FIND_WATCH (phone → watch): phone sends this to locate the watch.  The
//   watch vibrates with a short pulse every ~1 s for 30 s, then stops.
//   Accepts an optional CBOR map with "duration" (uint, seconds, capped at 60)
//   to override the default 30 s.  Older senders that omit the map get the
//   default.
// ---------------------------------------------------------------------------

void BridgeController::HandleFindWatch(const uint8_t* payload, size_t len, uint16_t msgId) {
  (void)msgId;

  SetBreadcrumb(Breadcrumb::CBOR_DECODE,
                static_cast<uint8_t>(MessageType::FIND_WATCH), msgId);

  // Optional CBOR map — "duration" key overrides the 30 s default.
  uint32_t durationMs = kFindWatchDurationMs;
  if (payload != nullptr && len > 0) {
    CborParser parser;
    CborValue root, mapIter;
    if (cbor_parser_init(payload, len, 0, &parser, &root) == CborNoError &&
        cbor_value_is_map(&root) &&
        cbor_value_enter_container(&root, &mapIter) == CborNoError) {
      while (!cbor_value_at_end(&mapIter)) {
        bool isDuration = false;
        if (cbor_value_is_text_string(&mapIter)) {
          cbor_value_text_string_equals(&mapIter, "duration", &isDuration);
        }
        cbor_value_advance(&mapIter);
        if (cbor_value_at_end(&mapIter)) break;
        if (isDuration && cbor_value_is_unsigned_integer(&mapIter)) {
          uint64_t secs = 0;
          cbor_value_get_uint64(&mapIter, &secs);
          // Cap at 60 s, then convert to OnTick iterations. This is the path
          // the phone actually uses — it always sends "duration" — so the
          // kFindWatchDurationMs default below is only a fallback for an
          // empty payload.
          if (secs > 60) secs = 60;
          durationMs = static_cast<uint32_t>(secs) * 1000u;
        }
        cbor_value_advance(&mapIter);
      }
    }
  }

  // Arm the alert: store the deadline and trigger the first vibration now.
  const uint32_t nowMs = NowMs();
  findWatchUntilMs_ = nowMs + durationMs;
  lastFindWatchVibrateMs_ = nowMs;

  RequestHaptic(HapticPattern::Double);

  // The repeating buzzes are driven by OnTick, which runs only while the screen is on, and a
  // lost watch is usually asleep. Turn the screen on as a notification does: no
  // wake lock, so it sleeps again after the usual timeout and the buzzing stops
  // with it, before the deadline if that comes first.
  //
  // The early sleep after a notification wake (HomeAssistantService) would turn the screen off
  // 5 s after a pop-up and stop the buzzing: end it, and keep it off for the length of the alert.
  bleService_.SuspendEarlySleep(durationMs);
  bleService_.WakeScreen();

  SetBreadcrumb(Breadcrumb::IDLE);
}

// ---------------------------------------------------------------------------
// REQ-068 — ACTION_RESULT handler (phone → watch).
//
// Wire format (CBOR map): { actionId: uint, ok: bool, message?: tstr }
//
// The phone sends one or two ACTION_RESULT messages per ACTION_INVOKE:
//   1. An immediate "queued" response (ok=true, message="queued") for all
//      long-running actions.  The watch ignores message=="queued" because
//      the ⌛ is already shown by OnActionInvoked.
//   2. A final result (ok=true or ok=false) once the HA call completes.
//
// The watch matches by actionId (not msgId), updates the cross-task signal
// in HomeAssistantService, and fires a short haptic on success or double on
// failure.  The display task (HomeAssistant::Refresh) reads the signal and
// overlays ✓ / ✗ on the tile for 2 seconds.
// ---------------------------------------------------------------------------
void BridgeController::HandleActionResult(const uint8_t* payload, size_t len) {
  if (payload == nullptr || len == 0) return;

  SetBreadcrumb(Breadcrumb::CBOR_DECODE,
                static_cast<uint8_t>(MessageType::ACTION_RESULT), 0);

  // Inline CBOR decode — ZERO heap allocation.
  CborParser parser;
  CborValue root, mapIter;
  if (cbor_parser_init(payload, len, 0, &parser, &root) != CborNoError) {
    SetBreadcrumb(Breadcrumb::IDLE);
    return;
  }
  if (!cbor_value_is_map(&root)) {
    SetBreadcrumb(Breadcrumb::IDLE);
    return;
  }
  if (cbor_value_enter_container(&root, &mapIter) != CborNoError) {
    SetBreadcrumb(Breadcrumb::IDLE);
    return;
  }

  uint32_t actionId = 0xFFFF;
  bool ok           = false;
  bool hasOk        = false;
  bool isQueued     = false;
  bool isDone       = false;   // message "done"
  bool isCancelled  = false;   // message "cancelled"

  while (!cbor_value_at_end(&mapIter)) {
    // Read key
    bool isActionId = false, isOk = false, isMessage = false;
    if (cbor_value_is_text_string(&mapIter)) {
      cbor_value_text_string_equals(&mapIter, "actionId", &isActionId);
      cbor_value_text_string_equals(&mapIter, "ok",       &isOk);
      cbor_value_text_string_equals(&mapIter, "message",  &isMessage);
    }
    if (cbor_value_advance(&mapIter) != CborNoError) break;
    if (cbor_value_at_end(&mapIter)) break;

    if (isActionId && cbor_value_is_unsigned_integer(&mapIter)) {
      uint64_t v = 0;
      cbor_value_get_uint64(&mapIter, &v);
      actionId = static_cast<uint32_t>(v);
    } else if (isOk && cbor_value_is_boolean(&mapIter)) {
      cbor_value_get_boolean(&mapIter, &ok);
      hasOk = true;
    } else if (isMessage && cbor_value_is_text_string(&mapIter)) {
      // Check if this is the interim "queued" response.
      bool eq = false;
      cbor_value_text_string_equals(&mapIter, "queued", &eq);
      isQueued = eq;
      cbor_value_text_string_equals(&mapIter, "done", &isDone);
      cbor_value_text_string_equals(&mapIter, "cancelled", &isCancelled);
    }
    if (cbor_value_advance(&mapIter) != CborNoError) break;
  }

  // Ignore the interim "queued" ack — ⌛ is already shown; wait for the final result.
  if (!hasOk || isQueued) {
    SetBreadcrumb(Breadcrumb::IDLE);
    return;
  }

  // A voice session's status (0xFFFE), whatever tile tap is or is not pending: no tile glyph is
  // touched and the tile pending state is left alone. "listening", "processing" and "speaking"
  // (ok) keep the session active and make no sound; "done", "cancelled" and any failure end it.
  // The final result's buzz (done: short, failure: double) is what the phone's own
  // HAPTIC_FEEDBACK asks for, and RequestHaptic plays the pair once.
  if (actionId == VOICE_ASSIST_ACTION_ID) {
    if (ok && !isDone && !isCancelled) {
      MarkVoiceSession(true, true);
    } else {
      MarkVoiceSession(false, false);
      if (!ok) {
        RequestHaptic(HapticPattern::Double, false, true);
      } else if (isDone) {
        RequestHaptic(HapticPattern::Short, false, true);
      }
    }
    SetBreadcrumb(Breadcrumb::IDLE);
    return;
  }

  // Confirm the actionId matches the pending invoke (defence against stale replies).
  if (actionId != static_cast<uint32_t>(pendingActionId_) && pendingActionId_ != 0xFFFF) {
    // Unknown / stale ACTION_RESULT — ignore silently.
    SetBreadcrumb(Breadcrumb::IDLE);
    return;
  }

  // Route result to the display task via HomeAssistantService atomic.
  // resultCode: 2 = ok (✓), 3 = fail (✗).
  const uint8_t resultCode = ok ? 2u : 3u;
  if (pendingActionTileIndex_ != 0xFF) {
    bleService_.SetActionResult(resultCode, pendingActionTileIndex_);
  }

  // Haptic feedback: short buzz on success, double on failure.
  RequestHaptic(ok ? HapticPattern::Short : HapticPattern::FailureDouble, true);

  // Clear pending state so subsequent stale replies are discarded.
  pendingActionId_        = 0xFFFF;
  pendingActionTileIndex_ = 0xFF;

  SetBreadcrumb(Breadcrumb::IDLE);
}

// ---------------------------------------------------------------------------
// REQ-070 — calendar-event start vibration.
//
// The display task sets bleService_.calendarVibratePending_ via
// RequestCalendarVibration() when it detects that a calendar event's
// eventStartEpoch has just been crossed.  This handler drains that flag and
// fires a Double haptic so the watch buzzes at event start without the display
// task ever touching the motor controller directly.
// ---------------------------------------------------------------------------
void BridgeController::ProcessCalendarVibration() {
  if (!bleService_.TakeCalendarVibration()) return;
  RequestHaptic(HapticPattern::Double, true);
}

void BridgeController::ProcessFindWatch() {
  const uint32_t nowMs = NowMs();
  // Signed comparison so the check is wrap-safe across the 32-bit ms rollover.
  if (findWatchUntilMs_ == 0 || static_cast<int32_t>(nowMs - findWatchUntilMs_) >= 0) {
    findWatchUntilMs_ = 0;
    return;
  }
  if (static_cast<uint32_t>(nowMs - lastFindWatchVibrateMs_) >= kFindWatchVibrateIntervalMs) {
    lastFindWatchVibrateMs_ = nowMs;
    // Only trigger if no other haptic is pending.
    RequestHaptic(HapticPattern::Short, true);
  }
}

} // namespace HaBridge
} // namespace Applications
} // namespace Pinetime
