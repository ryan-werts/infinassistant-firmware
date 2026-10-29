#include "ha_bridge/BootConfirm.h"

#if HA_BOOT_FIXES
#include <FreeRTOS.h>
#include <task.h>
#include <nrf.h>

// The watchdog is fed from the tick interrupt for the first 30 s of uptime, whatever the tasks
// are doing. SystemTask only feeds it from its loop, which it reaches after the whole start-up
// sequence; the boot buzzes used to feed it on the way and are gone. A start-up that hangs
// still resets the watch, 30 s later. configUSE_TICK_HOOK is 1 for every executable, and this
// library is linked by all of them.
extern "C" void vApplicationTickHook(void) {
  const TickType_t now = xTaskGetTickCountFromISR();
  if (now < static_cast<TickType_t>(30 * configTICK_RATE_HZ)) {
    NRF_WDT->RR[0] = WDT_RR_RR_Reload;
  }
#if HA_BOOT_BOX
  if ((now & 15u) == 0u) {
    HaBootBoxTick(now);
  }
#endif
}
#endif

#if HA_BOOT_BOX
// The recorder is in BootBox.cpp, which only the application links (see there). These stand in
// for it in the other executables: BridgeController, which they link too, calls the last two.
extern "C" __attribute__((weak)) void HaBootBoxTick(uint32_t /*tick*/) {
}
extern "C" __attribute__((weak)) bool HaBootBoxGetCrash(HaBootBoxCrash* /*out*/) {
  return false;
}
extern "C" __attribute__((weak)) void HaBootBoxAckCrash(void) {
}
#endif

#if HA_BOOT_TRACE
// The recorder lives with the crash context in BridgeController.cpp, which only the
// application links. Every executable links this library, so the recovery firmware and
// the loaders get this no-op instead.
extern "C" __attribute__((weak)) void HaBootMarkStep(uint8_t /*step*/) {
}
extern "C" __attribute__((weak)) void HaBootMarkInitStage(uint8_t /*stage*/) {
}
#endif

namespace Pinetime {
namespace Applications {
namespace HaBridge {

// Deliberately ordinary .bss, NOT .noinit: both must read false on every boot.
// A confirmation carried across a reset would mean an image that crash-looped
// could inherit "already confirmed" from the run before it, which is the whole
// failure this policy exists to stop.
std::atomic<bool> BootConfirm::firstScreenRendered_{false};
std::atomic<bool> BootConfirm::confirmed_{false};
std::atomic<bool> BootConfirm::firstReadHeld_{false};
std::atomic<bool> BootConfirm::firstReadTaken_{false};
std::atomic<int8_t> BootConfirm::validatedAtBoot_{-1};
std::atomic<int8_t> BootConfirm::wipeStoreAtBoot_{-1};

#if HA_BOOT_TRACE
void BootConfirm::MarkStep(uint8_t step) {
  HaBootMarkStep(step);
}

void BootConfirm::MarkInitStage(uint8_t stage) {
  HaBootMarkInitStage(stage);
}
#endif

}
}
}
