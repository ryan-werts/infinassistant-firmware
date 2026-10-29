#pragma once

#include <stdbool.h>
#include <stdint.h>

// Boot black box. A record in .noinit RAM, which survives resets and MCUboot, follows each boot of
// the application:
// - how far SystemTask's start-up got (the HA_INIT_STAGE stages, 1-12);
// - the display and sleep milestones, with the time of the latest;
// - the last times the tick interrupt, SystemTask's loop and the display task ran;
// - the battery voltage, the charger state and the lowest free heap;
// - how the boot ended, when that can be seen: a HardFault (with r4-r11, the call chain, the stack
//   left and a heap walk), the watchdog's interrupt (and whether the button was held, which is how
//   a user restarts the watch), an SDK error (APP_ERROR_CHECK), plus flags for a stack overflow or
//   a failed allocation.
// Nothing runs on the normal path beyond these stores. After a boot that ended badly (a fault, an
// SDK error, the watchdog with the button up, or a lockup), the next boot shows that record on the
// screen for its first minute (DisplayApp.cpp), two pages alternating every 10 s; the phone is not
// needed to read it. If that boot died before its first watch face read was drawn, the next boot
// also starts from an empty layout store (BootConfirm::WipeStoreAtBoot).
#ifndef HA_BOOT_BOX
#define HA_BOOT_BOX 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Milestones for HaBootBoxEvent, recorded as bit (1 << event) with the time of the latest.
enum {
  kHaBootEvFace = 1,       // DisplayApp: the first watch face constructed (LoadNewScreen returned)
  kHaBootEvConfirm = 2,    // BootConfirm wrote, or found, the confirmation after the first draw
  kHaBootEvDim = 3,        // DisplayApp: the display dimmed before sleep
  kHaBootEvSleepReq = 4,   // SystemTask: going to sleep
  kHaBootEvDisplayOff = 5, // DisplayApp: the LCD put to sleep
  kHaBootEvSleeping = 6,   // SystemTask: SPI flash, SPI and touch panel put to sleep
  kHaBootEvWake = 7,       // SystemTask: woken up
};

// main(), just before systemTask.Start(): keeps the previous boot's record when it ended badly,
// starts this boot's, and tells BootConfirm whether to wipe the layout store.
void HaBootBoxBegin(void);
// SystemTask start-up stage (the furthest one is kept).
void HaBootBoxStage(uint8_t stage);
// A milestone (kHaBootEv*). Task context only.
void HaBootBoxEvent(uint8_t event);
// Every SystemTask loop pass: the time, the battery and the lowest free heap.
// power: bit 0 charging, bit 1 power present.
void HaBootBoxLoop(uint16_t batteryMv, uint8_t power);
// Every DisplayApp refresh: the time.
void HaBootBoxDisplayTick(void);
// What the display task is doing, for the record of a fault: 1 the report label's update, 2 LVGL's
// task handler (drawing and the watch face's refresh task), 0 anything else.
void HaBootBoxPhase(uint8_t phase);
// From the FreeRTOS hooks in main.cpp.
void HaBootBoxStackOverflow(const char* taskName);
void HaBootBoxMallocFailed(void);
// From the tick hook (BootConfirm.cpp), every 16 ticks. A weak no-op there stands in for the
// executables that do not link the black box.
void HaBootBoxTick(uint32_t tick);
// The report on the previous boot, in lines of at most 20 characters, rebuilt by each call; the
// page changes every 10 s. NULL when the previous boot did not end badly: nothing to show.
const char* HaBootBoxReport(void);
// The report is off the screen for good: its memory is released.
void HaBootBoxReportDone(void);

// How the previous boot ended, for the phone's crash report (BridgeController's CRASH_DUMP reply):
// kept from main() when that boot ended badly, until the phone acknowledges it.
typedef struct {
  uint8_t kind;          // 1 HardFault, 2 watchdog, 3 SDK error; 0 a lockup or watchdog reset with no record
  uint8_t ctx;           // where it hit (the black box's context codes)
  uint8_t stage;         // furthest SystemTask start-up stage
  uint8_t flags;
  uint32_t uptimeTicks;  // the tick count of that boot's last tick-hook record
  uint32_t pc;
  uint32_t lr;
  uint32_t cfsr;         // fault: CFSR; SDK error: the error code
  uint32_t addr;         // fault: BFAR or MMFAR when valid
  uint32_t minHeap;
  uint32_t calls[2];     // the innermost return addresses above the fault frame
} HaBootBoxCrash;

// True with *out filled while the previous boot's bad end is unacknowledged. Weak in BootConfirm.cpp
// (false) for the executables that do not link the black box.
bool HaBootBoxGetCrash(HaBootBoxCrash* out);
// The phone has the crash report: stop offering it.
void HaBootBoxAckCrash(void);

#ifdef __cplusplus
}
#endif
