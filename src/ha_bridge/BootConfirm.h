#pragma once

#include <atomic>
#include <cstdint>
#include "components/firmwarevalidator/FirmwareValidator.h"
#include "ha_bridge/BootBox.h"

// Boot diagnostics, both off unless a diagnostic build turns them on:
// - HA_BOOT_TRACE: the boot steps between the face's construction and the confirmation
//   (BootConfirm::MarkStep) and SystemTask's start-up stages (MarkInitStage) are recorded in the
//   .noinit crash context, and the layout store is also wiped after a boot that never drew its
//   first read.
// - HA_FAULT_TRACE: a HardFault handler records the faulting PC, LR and CFSR there
//   (BridgeController.cpp).
// 2.4.74, with both on, failed to install where 2.4.73, with neither, installed; an older note
// in BridgeController.cpp records builds with their own HardFault handler boot-looping.
#ifndef HA_BOOT_TRACE
#define HA_BOOT_TRACE 0
#endif
#ifndef HA_FAULT_TRACE
#define HA_FAULT_TRACE 0
#endif

// Diagnostic (2.4.80): never write image_ok. Every boot that had to confirm itself (a first boot
// after an update, and every boot of a permanent install, where FirmwareValidator's whole-word
// check never matches MCUboot's flag byte) has died, while boots that did not have to confirm
// have run. With this on, a first boot that survives proves the confirmation is what kills it;
// the image stays unconfirmed and the next reset rolls it back.
#ifndef HA_BOOT_NO_CONFIRM
#define HA_BOOT_NO_CONFIRM 0
#endif

// 2.4.73/2.4.74 boot fixes: the watchdog is fed from the tick hook for the first 30 s of uptime
// (BootConfirm.cpp), and the layout store is wiped at boot when the previous boot died before
// its first store read was drawn (BridgeController::Init; see WipeStoreAtBoot).
#ifndef HA_BOOT_FIXES
#define HA_BOOT_FIXES 1
#endif

#if HA_BOOT_TRACE
// SystemTask start-up stage recorder (BridgeController.cpp; a no-op in BootConfirm.cpp for the
// executables that do not link the bridge).
extern "C" void HaBootMarkInitStage(uint8_t stage);
#endif

namespace Pinetime {
namespace Applications {
namespace HaBridge {

/**
 * MCUboot image confirmation policy.
 *
 * The bootloader gives us a rollback safety net and we were throwing it away.
 * From bootloader/README.md, its contract is:
 *
 *   "The bootloader starts the watchdog just before running the new firmware.
 *    This way, the watchdog will reset the MCU after ~7s because the firmware
 *    does not refresh it. Bootloader reverts to the previous version."
 *
 * and, describing exactly the state this firmware put itself in:
 *
 *   "New firmware does not run properly but sets the valid bit and refreshes
 *    the watchdog | The bootloader won't revert to the previous version
 *    because the valid flag is set | Wait for the battery to drain."
 *
 * The overlay used to write image_ok in the first milliseconds of
 * SystemTask::Work(), before the display task had even started. Any image that
 * boot-looped after that point — and several did while this dashboard was
 * being built — could therefore never be reverted, and recovery meant draining
 * the battery or a manual DFU. Confirming is supposed to mean "this image
 * works", so it has to happen after there is evidence of that.
 *
 * Two triggers, and only these:
 *
 *  1. HEALTHY. The first watch face rendered — we got past LoadNewScreen,
 *     which is precisely where the 2.4.41/2.4.42 loops died — and, when the
 *     face asked for it, the face's first store read has been drawn.
 *
 *     The Home Assistant face reads the layout store (littlefs, on the flash)
 *     to draw itself. Built during an unconfirmed boot it does NOT do that read
 *     inside LoadNewScreen: 2.4.62 did, and it reset in exactly that window
 *     (breadcrumb 4 set, 5 never reached), so MCUboot rolled it back. The face
 *     takes HoldForFirstRead() in its constructor and reads on its first
 *     refresh instead. The confirmation waits for that read to be drawn
 *     (ReleaseFirstRead), so a failure in the read still reverts the image.
 *     A face that is destroyed before its read happens releases the hold, so
 *     it cannot block the confirmation for good.
 *
 *     Beyond that, no other milestone.
 *
 *     2.4.51-2.4.55 ALSO required 30 s of uptime, and that is why every one of
 *     them was reverted regardless of what it contained. The bootloader arms a
 *     ~7 s watchdog before running a NEW, unconfirmed image
 *     (bootloader/README.md: "starts the watchdog just before running the new
 *     firmware... the watchdog will reset the MCU after ~7s"), so a confirm
 *     gated on 30 s can never be reached on the boot that matters. The watch
 *     reset mid-LoadNewScreen — two buzzes, never three — and MCUboot rolled
 *     back, which is exactly what was observed on five dissimilar builds.
 *
 *     An already-confirmed image is not run as a test image, which is why
 *     2.4.50 boots to three buzzes today while nothing flashed after it stuck.
 *
 *  2. DFU STARTING. A new image is about to be written to the secondary slot.
 *     This is the case the old always-confirm-early policy existed to prevent:
 *     an unconfirmed crash-looping image plus a recovery DFU leaves MCUboot
 *     trying to revert into a slot that has since been overwritten, which
 *     corrupts the swap trailer (docs/howto/troubleshooting.md, the 2.4.11-
 *     2.4.16 era). Confirming the running image at that moment removes the
 *     revert intent before the slot changes under it, so the hazard is closed
 *     without giving up rollback for every other failure.
 */
class BootConfirm {
public:
  /**
   * The trailer address is NOT defined here. Stock InfiniTime already owns it
   * as FirmwareValidator::validBitAdress {0x7BFE8} / validBitValue {1}, and
   * this policy delegates to FirmwareValidator::Validate(). The overlay used
   * to spell 0x7BFE8 inline at two separate call sites, so a bootloader or
   * partition-map change would have turned those into blind writes somewhere
   * inside the image; there is no reason to keep a private copy of a constant
   * the platform already declares. Validate() is also a no-op when the bit is
   * already set, so it is safe to call more than once.
   */


  /** Called from DisplayApp once the first screen has actually been loaded. */
  static void NotifyFirstScreenRendered() {
    firstScreenRendered_.store(true, std::memory_order_release);
  }

  /**
   * Called by the HA face's constructor. True when this face must hold the
   * confirmation until its first store read is drawn: only for an image not yet
   * confirmed, and only for the first face built this boot (one token per boot,
   * so a later face, which builds long after the boot window, never holds).
   * The holder calls ReleaseFirstRead() when that read is drawn, or when it is
   * destroyed before it.
   */
  static bool HoldForFirstRead() {
    if (confirmed_.load(std::memory_order_acquire)) {
      return false;
    }
    bool expected = false;
    if (!firstReadTaken_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      return false;
    }
    firstReadHeld_.store(true, std::memory_order_release);
    return true;
  }

  /** The hold taken by HoldForFirstRead() ends: the read was drawn, or the face is gone. */
  static void ReleaseFirstRead() {
    firstReadHeld_.store(false, std::memory_order_release);
  }

  /**
   * Called from the SystemTask loop. Confirms as soon as the display task
   * reports a rendered face (and any first-read hold has ended). Cheap enough to call every iteration: after the
   * first success it is a single relaxed atomic load.
   */
  static void ConfirmIfHealthy() {
    if (confirmed_.load(std::memory_order_acquire)) {
      return;
    }
    if (!firstScreenRendered_.load(std::memory_order_acquire)) {
      return;
    }
    if (firstReadHeld_.load(std::memory_order_acquire)) {
      return;
    }
    Write();
  }

  /**
   * Called when a DFU is about to write the secondary slot. Unconditional:
   * whatever state this image is in, it must not be left as a revert target
   * pointing at a slot that is being overwritten.
   */
  static void ConfirmBeforeDfu() {
    Write();
  }

#if HA_BOOT_TRACE
  /**
   * Diagnostic: record that this boot reached `step` in the .noinit crash context (the
   * high nibble of its breadcrumb, with the time and the free heap). 1 face constructor
   * entered, 2 constructor done, 3 first refresh, 4 first tick done, 5 first read started,
   * 6 first read drawn, 7 about to confirm, 8 confirmed.
   */
  static void MarkStep(uint8_t step);

  /** SystemTask start-up stage, kept apart from the display task's steps. */
  static void MarkInitStage(uint8_t stage);
#endif

  /** Whether this boot has written image_ok yet (surfaced in diagnostics). */
  static bool IsConfirmed() {
    return confirmed_.load(std::memory_order_acquire);
  }

  /**
   * Whether image_ok was already set when this boot started: the state the boot black box found
   * in main() (SetValidatedAtBoot), or, without it, the live state.
   */
  static bool ValidatedAtBoot() {
    const int8_t atBoot = validatedAtBoot_.load(std::memory_order_acquire);
    if (atBoot >= 0) {
      return atBoot != 0;
    }
    return Pinetime::Controllers::FirmwareValidator().IsValidated();
  }

  static void SetValidatedAtBoot(bool validated) {
    validatedAtBoot_.store(validated ? 1 : 0, std::memory_order_release);
  }

  /**
   * Whether BridgeController::Init starts from an empty layout store. The boot black box decides
   * in main() (SetWipeStoreAtBoot): only when the previous boot ended badly before its first
   * store read was drawn, so a store that kills the boot is read once, and a normal update keeps
   * the layout. Without the black box: after every update (the image not confirmed yet).
   */
  static bool WipeStoreAtBoot() {
    const int8_t wipe = wipeStoreAtBoot_.load(std::memory_order_acquire);
    if (wipe >= 0) {
      return wipe != 0;
    }
    return !ValidatedAtBoot();
  }

  static void SetWipeStoreAtBoot(bool wipe) {
    wipeStoreAtBoot_.store(wipe ? 1 : 0, std::memory_order_release);
  }

private:
  static void Write() {
    // exchange, not store: ConfirmBeforeDfu can race the SystemTask loop, and
    // a flash write is not something to do twice.
    if (confirmed_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
#if HA_BOOT_TRACE
    MarkStep(7);
#endif
#if !HA_BOOT_NO_CONFIRM
    Pinetime::Controllers::FirmwareValidator validator;
    validator.Validate();
#endif
#if HA_BOOT_TRACE
    MarkStep(8);
#endif
#if HA_BOOT_BOX && !defined(PINETIME_IS_RECOVERY)
    HaBootBoxEvent(kHaBootEvConfirm);
#endif
  }

  static std::atomic<bool> firstScreenRendered_;
  static std::atomic<bool> confirmed_;
  static std::atomic<bool> firstReadHeld_;
  static std::atomic<bool> firstReadTaken_;
  static std::atomic<int8_t> validatedAtBoot_; // -1 until recorded
  static std::atomic<int8_t> wipeStoreAtBoot_; // -1 until decided
};

}
}
}
