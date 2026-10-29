#include "ha_bridge/BootBox.h"

#if HA_BOOT_BOX
#include <cstring>
#include <FreeRTOS.h>
#include <task.h>
#include <nrf.h>
#include <hal/nrf_gpio.h>
#include <nrf_delay.h>
#include <libraries/util/app_error.h>
#include "drivers/PinMap.h"
#include "components/firmwarevalidator/FirmwareValidator.h"
#include "ha_bridge/BootConfirm.h"
#include "ha_bridge/WakeTelemetry.h"

// Only the application links this file: every reference to it is compiled out of the recovery
// builds (PINETIME_IS_RECOVERY), and the overlay's libraries, which they also link, must not
// reference it. Its strong HardFault_Handler, WDT_IRQHandler and app_error_fault_handler replace
// the defaults in the application and nowhere else.

extern "C" {
// r4-r11 at a HardFault, stored by the handler before any compiled code can change them.
uint32_t g_haBootRegs[8] __attribute__((section(".noinit"), used));

// The heap walk (FreeRTOS/heap_4_infinitime.c): 0 when every block and the free list are
// consistent, else the address of the first bad block header; info gets its two words, the
// previous block's address and that block's size word. suspend: 0 only where nothing else can
// run (the fault handler).
uint32_t ulPortHeapCheck(uint32_t* info, int suspend);
}

namespace {

// Changed whenever the record's layout changes, so a record left by another layout is never read.
constexpr uint32_t kMagic = 0xB0070B11u;

// Flags.
constexpr uint8_t kValidatedAtBoot = 0x01; // image_ok was already set when the boot started
constexpr uint8_t kButtonHeld = 0x02;      // the watchdog fired with the button held: a user restart
constexpr uint8_t kStackOverflow = 0x20;
constexpr uint8_t kMallocFailed = 0x40;

// How a boot ended, when it was seen.
constexpr uint8_t kKindNone = 0;
constexpr uint8_t kKindFault = 1;    // HardFault (every fault escalates to it: the others are not enabled)
constexpr uint8_t kKindWatchdog = 2; // the watchdog's TIMEOUT interrupt, just before its reset
constexpr uint8_t kKindSdkError = 3; // APP_ERROR_CHECK and friends, which reset the watch

struct Record {
  uint32_t magic;
  uint16_t seq;        // boots since the record started
  uint8_t stage;       // furthest SystemTask start-up stage
  uint8_t kind;
  uint16_t events;     // milestones reached, bit (1 << kHaBootEv*)
  uint8_t lastEvent;
  uint8_t flags;
  uint8_t ctx;         // where the fault or the watchdog's interrupt hit: 1 displayapp, 2 MAIN,
                       // 3 ble, 4 ll, 5 Tmr Svc, 6 Heartrate, 7 other task, 8 interrupt,
                       // 9 before the scheduler, 10 IDLE
  uint8_t overflowCtx; // the task whose stack overflowed, same codes
  uint8_t power;       // bit 0 charging, bit 1 power present
  uint8_t uiPhase;     // the display task's phase (HaBootBoxPhase): 1 report label, 2 LVGL tasks
  uint16_t batteryMv;
  uint16_t ipsr;       // exception number when ctx is 8
  uint32_t resetReas;  // RESETREAS at the start of this boot: how the boot before it ended
  uint32_t tickAt;     // tick count at the last tick-hook record
  uint32_t loopAt;     // ... at the last SystemTask loop pass
  uint32_t displayAt;  // ... at the last DisplayApp refresh
  uint32_t eventAt;    // ... at the last milestone
  uint32_t pc;         // fault and watchdog: stacked PC; SDK error: its pc argument
  uint32_t lr;         // fault and watchdog: stacked LR; SDK error: the fault id
  uint32_t info;       // fault: CFSR; SDK error: the error code
  uint32_t info2;      // fault: HFSR; SDK error: the line, when the build records one
  uint32_t addr;       // fault: BFAR or MMFAR when valid; SDK error: the caller found on its stack
  uint32_t minHeap;    // lowest free heap ever, bytes
  uint32_t sp;         // fault: the exception frame's address
  uint32_t calls[6];   // fault: return addresses found on the stack above the frame, innermost first
  uint16_t stackFree;  // fault: the faulting task's stack high-water mark, in words
  uint8_t heapAtFaultBad; // fault: the heap walk found a bad block
  uint8_t pad;
  uint32_t check;      // ~magic
};

// main() clears the whole .noinit area when its own magic word is wrong, before
// HaBootBoxBegin() runs, so a record from another image never survives as valid.
Record g_box __attribute__((section(".noinit")));

// The previous boot's record and its registers, with the report's text: allocated only after a
// boot that ended badly, and released once the report is off the screen (HaBootBoxReportDone).
struct Prev {
  Record record;
  uint32_t regs[8];
  char text[256];
};
Prev* g_prev = nullptr;

// The previous boot's bad end, compact, for the phone's crash report; kept until it is acknowledged.
HaBootBoxCrash g_crash;
bool g_crashValid = false;

constexpr uintptr_t kSlot = 0x8000;
constexpr uint32_t kSlotSize = 0x74000;

// FreeRTOS keeps configMAX_TASK_NAME_LEN - 1 characters of a task's name (3 here).
uint8_t ContextOf(const char* name) {
  struct TaskCode {
    const char* name;
    uint8_t code;
  };
  static constexpr TaskCode kTasks[] = {
    {"displayapp", 1}, {"MAIN", 2}, {"ble", 3}, {"ll", 4}, {"Tmr Svc", 5}, {"Heartrate", 6}, {"IDLE", 10}};
  if (name == nullptr) {
    return 7;
  }
  for (const auto& task : kTasks) {
    if (std::strncmp(name, task.name, configMAX_TASK_NAME_LEN - 1) == 0) {
      return task.code;
    }
  }
  return 7;
}

// ipsr: the IPSR of the context in question (a stacked xPSR, or the live one).
uint8_t ContextNow(uint32_t ipsr) {
  if ((ipsr & 0x1FFu) != 0u) {
    return 8;
  }
  if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
    return 9;
  }
  return ContextOf(pcTaskGetName(nullptr));
}

void Backlight(bool on) {
  using namespace Pinetime::PinMap;
  // Active low.
  if (on) {
    nrf_gpio_pin_clear(LcdBacklightLow);
    nrf_gpio_pin_clear(LcdBacklightMedium);
    nrf_gpio_pin_clear(LcdBacklightHigh);
  } else {
    nrf_gpio_pin_set(LcdBacklightLow);
    nrf_gpio_pin_set(LcdBacklightMedium);
    nrf_gpio_pin_set(LcdBacklightHigh);
  }
}

void BacklightPins() {
  using namespace Pinetime::PinMap;
  nrf_gpio_cfg_output(LcdBacklightLow);
  nrf_gpio_cfg_output(LcdBacklightMedium);
  nrf_gpio_cfg_output(LcdBacklightHigh);
}

// Whether the boot a record describes ended in a way worth showing: a fault, an SDK error, the
// watchdog with the button up (held, the button is how a user restarts the watch), or a lockup or
// watchdog reset that left no record of its own. resetReas: how that boot ended (RESETREAS at the
// start of the next one).
bool EndedBadly(const Record& r, uint32_t resetReas) {
  switch (r.kind) {
    case kKindFault:
    case kKindSdkError:
      return true;
    case kKindWatchdog:
      return (r.flags & kButtonHeld) == 0u;
    default:
      return (resetReas & (POWER_RESETREAS_LOCKUP_Msk | POWER_RESETREAS_DOG_Msk)) != 0u;
  }
}

// Whether the halfwords just before a return address are a call (BL, BLX immediate or register).
bool IsAfterCall(uint32_t returnAddress) {
  const auto* h = reinterpret_cast<const uint16_t*>(returnAddress);
  const uint16_t first = h[-2];
  const uint16_t second = h[-1];
  if ((first & 0xF800u) == 0xF000u && (second & 0xC000u) == 0xC000u) {
    return true;
  }
  return (second & 0xFF87u) == 0x4780u;
}

// Report formatting, without printf.
struct Writer {
  char* p;
  char* end;
};

void Put(Writer& w, const char* s) {
  while (*s != '\0' && w.p < w.end) {
    *w.p++ = *s++;
  }
}

void PutU(Writer& w, uint32_t v) {
  char digits[10];
  unsigned n = 0;
  do {
    digits[n++] = static_cast<char>('0' + v % 10u);
    v /= 10u;
  } while (v != 0u);
  while (n != 0u && w.p < w.end) {
    *w.p++ = digits[--n];
  }
}

void PutX(Writer& w, uint32_t v, unsigned width) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  for (unsigned i = width; i != 0u && w.p < w.end; i--) {
    *w.p++ = kHex[(v >> (4u * (i - 1u))) & 0xFu];
  }
}

// Seconds with one decimal.
void PutT(Writer& w, uint32_t ticks) {
  const uint32_t tenths = static_cast<uint32_t>((static_cast<uint64_t>(ticks) * 10u) / configTICK_RATE_HZ);
  PutU(w, tenths / 10u);
  Put(w, ".");
  PutU(w, tenths % 10u);
}

// D watchdog, L lockup, S soft reset, P reset pin, O wake from System OFF, 0 none (power-on or
// brown-out, or cleared before the image ran).
const char* ResetCode(uint32_t reas) {
  if ((reas & POWER_RESETREAS_LOCKUP_Msk) != 0u) {
    return "L";
  }
  if ((reas & POWER_RESETREAS_DOG_Msk) != 0u) {
    return "D";
  }
  if ((reas & POWER_RESETREAS_SREQ_Msk) != 0u) {
    return "S";
  }
  if ((reas & POWER_RESETREAS_RESETPIN_Msk) != 0u) {
    return "P";
  }
  if ((reas & 0x000F0000u) != 0u) {
    return "O";
  }
  return "0";
}

const char* KindCode(uint8_t kind) {
  switch (kind) {
    case kKindFault:
      return "FLT";
    case kKindWatchdog:
      return "WDT";
    case kKindSdkError:
      return "ERR";
    default:
      return "---";
  }
}

void PutPair(Writer& w, uint32_t a, uint32_t b) {
  PutX(w, a, 8);
  Put(w, " ");
  PutX(w, b, 8);
}

// Page A: this boot (T: not confirmed yet, a test boot that a failure rolls back), then how the
// previous boot ended, when, and where.
void PageA(Writer& w, const Record& r) {
  Put(w, "#");
  PutU(w, g_box.seq);
  Put(w, " ");
  Put(w, ResetCode(g_box.resetReas));
  Put(w, " ");
  PutT(w, xTaskGetTickCount());
  Put(w, " s");
  PutU(w, g_box.stage);
  if ((g_box.flags & kValidatedAtBoot) == 0u) {
    Put(w, " T");
  }
  Put(w, "\nprev s");
  PutU(w, r.stage);
  Put(w, " e");
  PutX(w, r.events, 2);
  Put(w, " ");
  PutU(w, r.lastEvent);
  Put(w, "@");
  PutT(w, r.eventAt);
  Put(w, "\nt");
  PutT(w, r.tickAt);
  Put(w, " l");
  PutT(w, r.loopAt);
  Put(w, " d");
  PutT(w, r.displayAt);
  Put(w, "\n");
  Put(w, KindCode(r.kind));
  Put(w, " c");
  PutU(w, r.ctx);
  Put(w, " i");
  PutU(w, r.ipsr);
  Put(w, " f");
  PutX(w, r.flags, 2);
  Put(w, " ph");
  PutU(w, r.uiPhase);
  Put(w, "\npc ");
  PutX(w, r.pc, 8);
  if (r.heapAtFaultBad != 0u) {
    Put(w, " HEAP");
  }
  Put(w, "\nlr ");
  PutX(w, r.lr, 8);
  Put(w, "\n");
  PutPair(w, r.info, r.info2);
  Put(w, "\na");
  PutX(w, r.addr, 8);
  Put(w, " h");
  PutU(w, r.minHeap);
  Put(w, "\nb");
  PutU(w, r.batteryMv);
  Put(w, " p");
  PutU(w, r.power);
  Put(w, " o");
  PutU(w, r.overflowCtx);
  Put(w, " r");
  PutX(w, r.resetReas, 5);
}

// Page B: the previous boot's fault in detail: frame address and stack left, r4-r11, the call chain.
void PageB(Writer& w, const Record& r, const uint32_t* regs) {
  Put(w, "#");
  PutU(w, g_box.seq);
  Put(w, " B\nsp");
  PutX(w, r.sp, 8);
  Put(w, " w");
  PutU(w, r.stackFree);
  for (unsigned i = 0; i < 8; i += 2) {
    Put(w, "\n");
    PutPair(w, regs[i], regs[i + 1]);
  }
  for (unsigned i = 0; i < 6; i += 2) {
    Put(w, "\n");
    PutPair(w, r.calls[i], r.calls[i + 1]);
  }
}

} // namespace

extern "C" {

void HaBootBoxBegin(void) {
  // Read, not cleared: the watchdog driver clears it later in SystemTask::Work.
  const uint32_t reas = NRF_POWER->RESETREAS;
  const bool havePrev = g_box.magic == kMagic && g_box.check == ~kMagic;
  bool wipe = false;
  if (havePrev && EndedBadly(g_box, reas)) {
    // A layout store that a boot died reading must not be read again: the boot ended before its
    // first watch face read was drawn (the confirmation milestone comes after it).
    wipe = (g_box.events & (1u << kHaBootEvConfirm)) == 0u;
    g_crash.kind = g_box.kind;
    g_crash.ctx = g_box.ctx;
    g_crash.stage = g_box.stage;
    g_crash.flags = g_box.flags;
    g_crash.uptimeTicks = g_box.tickAt;
    g_crash.pc = g_box.pc;
    g_crash.lr = g_box.lr;
    g_crash.cfsr = g_box.info;
    g_crash.addr = g_box.addr;
    g_crash.minHeap = g_box.minHeap;
    g_crash.calls[0] = g_box.calls[0];
    g_crash.calls[1] = g_box.calls[1];
    g_crashValid = true;
    // From the heap before the scheduler starts, as the task stacks that systemTask.Start()
    // allocates next.
    g_prev = static_cast<Prev*>(pvPortMalloc(sizeof(Prev)));
    if (g_prev != nullptr) {
      g_prev->record = g_box;
      std::memcpy(g_prev->regs, g_haBootRegs, sizeof(g_prev->regs));
    }
  }
  Pinetime::Applications::HaBridge::BootConfirm::SetWipeStoreAtBoot(wipe);
  std::memset(g_haBootRegs, 0, sizeof(g_haBootRegs));
  const uint16_t seq = havePrev ? static_cast<uint16_t>(g_box.seq + 1u) : 1u;
  std::memset(&g_box, 0, sizeof(g_box));
  g_box.seq = seq;
  g_box.resetReas = reas;
  const bool validated = Pinetime::Controllers::FirmwareValidator().IsValidated();
  Pinetime::Applications::HaBridge::BootConfirm::SetValidatedAtBoot(validated);
  if (validated) {
    g_box.flags |= kValidatedAtBoot;
  }
  g_box.magic = kMagic;
  g_box.check = ~kMagic;
  // The watchdog's TIMEOUT interrupt comes two 32.768 kHz cycles before its reset, long enough
  // to record where the CPU was. Priority 0, so neither a stuck interrupt handler of lower
  // priority nor a FreeRTOS critical section (BASEPRI) holds it off. The bootloader already
  // runs the watchdog.
  NRF_WDT->INTENSET = WDT_INTENSET_TIMEOUT_Msk;
  NVIC_SetPriority(WDT_IRQn, 0);
  NVIC_ClearPendingIRQ(WDT_IRQn);
  NVIC_EnableIRQ(WDT_IRQn);
}

void HaBootBoxStage(uint8_t stage) {
  if (stage > g_box.stage) {
    g_box.stage = stage;
  }
}

void HaBootBoxEvent(uint8_t event) {
  g_box.events = static_cast<uint16_t>(g_box.events | (1u << event));
  g_box.lastEvent = event;
  g_box.eventAt = xTaskGetTickCount();
  // The screen-wake telemetry (WakeTelemetry.h) takes its screen-on and screen-off times from the
  // two milestones SystemTask already reports: its wake, and the move to sleep it finishes. Both are
  // SystemTask's own; with HA_BOOT_BOX off these two counters stay at their start values.
  // The HELLO reads the counters from the BLE host task, inside a critical section
  // (HomeAssistantService::ReadWakeDiag). The write is one as well, so a read that preempts
  // SystemTask halfway cannot see the session flag and its start time out of step.
  if (event == kHaBootEvWake) {
    const uint32_t nowMs = Pinetime::Applications::HaBridge::WakeTicksToMs(xTaskGetTickCount(), configTICK_RATE_HZ);
    taskENTER_CRITICAL();
    Pinetime::Applications::HaBridge::g_wakeTelemetry.OnScreenOn(nowMs);
    taskEXIT_CRITICAL();
  } else if (event == kHaBootEvSleeping) {
    const uint32_t nowMs = Pinetime::Applications::HaBridge::WakeTicksToMs(xTaskGetTickCount(), configTICK_RATE_HZ);
    taskENTER_CRITICAL();
    Pinetime::Applications::HaBridge::g_wakeTelemetry.OnScreenOff(nowMs);
    taskEXIT_CRITICAL();
  }
}

void HaBootBoxLoop(uint16_t batteryMv, uint8_t power) {
  g_box.loopAt = xTaskGetTickCount();
  g_box.batteryMv = batteryMv;
  g_box.power = power;
  g_box.minHeap = static_cast<uint32_t>(xPortGetMinimumEverFreeHeapSize());
}

void HaBootBoxDisplayTick(void) {
  g_box.displayAt = xTaskGetTickCount();
}

void HaBootBoxPhase(uint8_t phase) {
  g_box.uiPhase = phase;
}

void HaBootBoxStackOverflow(const char* taskName) {
  g_box.flags |= kStackOverflow;
  if (g_box.overflowCtx == 0u) {
    g_box.overflowCtx = ContextOf(taskName);
  }
}

void HaBootBoxMallocFailed(void) {
  g_box.flags |= kMallocFailed;
}

void HaBootBoxTick(uint32_t tick) {
  g_box.tickAt = tick;
}

const char* HaBootBoxReport(void) {
  if (g_prev == nullptr) {
    return nullptr;
  }
  Writer w {g_prev->text, g_prev->text + sizeof(g_prev->text) - 1};
  if (((xTaskGetTickCount() / (10u * configTICK_RATE_HZ)) & 1u) == 0u) {
    PageA(w, g_prev->record);
  } else {
    PageB(w, g_prev->record, g_prev->regs);
  }
  *w.p = '\0';
  return g_prev->text;
}

void HaBootBoxReportDone(void) {
  vPortFree(g_prev);
  g_prev = nullptr;
}

// Called on the NimBLE host task (BridgeController's CRASH_DUMP handlers); g_crash is written once,
// in main(), before the scheduler starts.
bool HaBootBoxGetCrash(HaBootBoxCrash* out) {
  if (!g_crashValid || out == nullptr) {
    return false;
  }
  *out = g_crash;
  return true;
}

void HaBootBoxAckCrash(void) {
  g_crashValid = false;
}

// Fault: record, then hold the backlight on, so the last frame stays readable, until the watchdog
// resets the watch (at most 7 s: the tick interrupt that feeds it cannot run).
__attribute__((used, noreturn)) void HaBootBoxFault(const uint32_t* frame, uint32_t excReturn) {
  g_box.kind = kKindFault;
  g_box.ipsr = static_cast<uint16_t>(frame[7] & 0x1FFu);
  g_box.ctx = ContextNow(frame[7]);
  g_box.pc = frame[6];
  g_box.lr = frame[5];
  const uint32_t cfsr = SCB->CFSR;
  g_box.info = cfsr;
  g_box.info2 = SCB->HFSR;
  if ((cfsr & (1u << 15)) != 0u) {
    g_box.addr = SCB->BFAR;
  } else if ((cfsr & (1u << 7)) != 0u) {
    g_box.addr = SCB->MMFAR;
  }
  g_box.sp = reinterpret_cast<uint32_t>(frame);
  // The call chain: return addresses on the stack above the exception frame (8 words, or 26 with
  // the FPU state, plus the alignment word when xPSR bit 9 says one was inserted).
  const uint32_t* p = frame + (((excReturn & 0x10u) != 0u) ? 8 : 26);
  if ((frame[7] & (1u << 9)) != 0u) {
    p++;
  }
  unsigned found = 0;
  for (unsigned i = 0; i < 160 && found < 6 && reinterpret_cast<uintptr_t>(p) < 0x20010000u; i++, p++) {
    const uint32_t v = *p;
    if ((v & 1u) != 0u && v > kSlot + 4u && v < kSlot + kSlotSize && IsAfterCall(v & ~1u)) {
      g_box.calls[found++] = v;
    }
  }
  uint32_t heapInfo[4] = {};
  if (ulPortHeapCheck(heapInfo, 0) != 0u) {
    g_box.heapAtFaultBad = 1;
  }
  if ((excReturn & 0x4u) != 0u && xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
    g_box.stackFree = static_cast<uint16_t>(uxTaskGetStackHighWaterMark(nullptr));
  }
  BacklightPins();
  Backlight(true);
  for (;;) {
  }
}

// r4-r11 first, before compiled code can touch them; then the frame (EXC_RETURN bit 2 says which
// stack holds it) and EXC_RETURN itself (bit 4 says whether the FPU state was stacked).
__attribute__((naked)) void HardFault_Handler(void) {
  __asm volatile("movw  r1, #:lower16:g_haBootRegs \n"
                 "movt  r1, #:upper16:g_haBootRegs \n"
                 "stmia r1, {r4-r11}               \n"
                 "tst   lr, #4                     \n"
                 "ite   eq                         \n"
                 "mrseq r0, msp                    \n"
                 "mrsne r0, psp                    \n"
                 "mov   r1, lr                     \n"
                 "b     HaBootBoxFault             \n");
}

__attribute__((used)) void HaBootBoxWatchdog(const uint32_t* frame) {
  NRF_WDT->EVENTS_TIMEOUT = 0;
  if (g_box.kind == kKindNone) {
    g_box.kind = kKindWatchdog;
    g_box.ipsr = static_cast<uint16_t>(frame[7] & 0x1FFu);
    g_box.ctx = ContextNow(frame[7]);
    g_box.pc = frame[6];
    g_box.lr = frame[5];
    // SystemTask stops feeding the watchdog while the button is held (it reads 1 when pressed):
    // that is how a user restarts the watch, and it is not worth a report.
    if (nrf_gpio_pin_read(Pinetime::PinMap::Button) != 0u) {
      g_box.flags |= kButtonHeld;
    }
  }
}

__attribute__((naked)) void WDT_IRQHandler(void) {
  __asm volatile("tst   lr, #4               \n"
                 "ite   eq                   \n"
                 "mrseq r0, msp              \n"
                 "mrsne r0, psp              \n"
                 "b     HaBootBoxWatchdog    \n");
}

// Replaces the SDK's weak handler, which in a release build disables interrupts and resets
// the watch without a trace. Same end, with a record and 3 s of steady backlight first.
void app_error_fault_handler(uint32_t id, uint32_t pc, uint32_t info) {
  __disable_irq();
  const uint32_t ipsr = __get_IPSR();
  g_box.kind = kKindSdkError;
  g_box.ipsr = static_cast<uint16_t>(ipsr & 0x1FFu);
  g_box.ctx = ContextNow(ipsr);
  g_box.pc = pc;
  g_box.lr = id;
  g_box.info = 0;
  g_box.info2 = 0;
  g_box.addr = 0;
  if (info != 0u && id == NRF_FAULT_ID_SDK_ERROR) {
    const auto* error = reinterpret_cast<const error_info_t*>(info);
    g_box.info = error->err_code;
    g_box.info2 = error->line_num;
    // A release build passes neither file, line nor pc. The error_info_t sits in
    // app_error_handler_bare's frame, and that frame's saved LR, the caller's return address,
    // is a few words above it: keep the first word there that looks like a Thumb address in
    // the image.
    const auto* above = reinterpret_cast<const uint32_t*>(info + sizeof(error_info_t));
    for (unsigned i = 0; i < 6; i++) {
      const uint32_t v = above[i];
      if ((v & 1u) != 0u && v > kSlot && v < kSlot + kSlotSize) {
        g_box.addr = v;
        break;
      }
    }
  } else if (info != 0u && id == NRF_FAULT_ID_SDK_ASSERT) {
    g_box.info2 = reinterpret_cast<const assert_info_t*>(info)->line_num;
  }
  BacklightPins();
  Backlight(true);
  nrf_delay_ms(3000);
  NVIC_SystemReset();
}

} // extern "C"

#endif // HA_BOOT_BOX
