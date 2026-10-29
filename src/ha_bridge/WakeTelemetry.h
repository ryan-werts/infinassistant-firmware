#pragma once

#include <cstddef>
#include <cstdint>

namespace Pinetime {
  namespace Applications {
    namespace HaBridge {

      /**
       * Where the screen time goes (2.4.104). Header-only and free of FreeRTOS, so the host tests
       * (firmware/tests/test_wake_telemetry.cpp) run the code the watch runs. Three parts:
       *
       *  - WakeTelemetry: the counters since boot that HELLO reports (WakeDiag.h).
       *  - NotifyWakeGate: what a notification does to a screen that is off.
       *  - NotifyWakeSleepWatch: puts the screen to sleep early after a wake that only a
       *    notification caused.
       *
       * All times are milliseconds modulo 2^32 and are only ever subtracted.
       */

      // The ticks of a clock of `hz` ticks a second, in milliseconds. The 64-bit product cannot
      // overflow; the result is the millisecond count modulo 2^32.
      inline uint32_t WakeTicksToMs(uint32_t ticks, uint32_t hz) {
        return static_cast<uint32_t>((static_cast<uint64_t>(ticks) * 1000u) / hz);
      }

      struct WakeCounters {
        uint32_t wakes {0};         // times the screen came on since boot (the boot itself is not one)
        uint32_t notifyWakes {0};   // of those, the ones a notification caused
        uint32_t notifyStored {0};  // notifications stored in the list without waking the screen
        uint32_t screenOnSec {0};   // seconds the screen has been on since boot
      };

      /**
       * The screen sessions and the notification counts.
       *
       * OnScreenOn / OnScreenOff come from SystemTask (BootBox.cpp, where the black box already
       * hooks the wake and the screen-off milestones); the two counters from the BLE task. The two
       * writes, the two counts and Read are each called inside a critical section (BootBox.cpp,
       * HomeAssistantService), so the pair `on_` and `onSince_` is never seen half written, whichever
       * task preempts.
       *
       * The watch boots with the screen on: that session counts for the time but is not a wake. A
       * second OnScreenOn while a session is open (a wake during the move to sleep, before the
       * screen went off) is the same session and is not counted again.
       */
      class WakeTelemetry {
       public:
        void OnScreenOn(uint32_t nowMs) {
          if (on_) return;
          on_ = true;
          onSince_ = nowMs;
          ++wakes_;
        }

        void OnScreenOff(uint32_t nowMs) {
          if (!on_) return;
          on_ = false;
          onMs_ += static_cast<uint32_t>(nowMs - onSince_);
        }

        void CountNotifyWake() { ++notifyWakes_; }
        void CountNotifyStored() { ++notifyStored_; }

        WakeCounters Read(uint32_t nowMs) const {
          WakeCounters c;
          c.wakes = wakes_;
          c.notifyWakes = notifyWakes_;
          c.notifyStored = notifyStored_;
          uint32_t ms = onMs_;
          if (on_) ms += static_cast<uint32_t>(nowMs - onSince_);
          c.screenOnSec = ms / 1000u;
          return c;
        }

       private:
        bool on_ {true};
        uint32_t onSince_ {0};
        uint32_t onMs_ {0};
        uint32_t wakes_ {0};
        uint32_t notifyWakes_ {0};
        uint32_t notifyStored_ {0};
      };

      // The one instance: SystemTask's wake and sleep events write it (BootBox.cpp), the notification
      // path counts into it and the HELLO reads it (HomeAssistantService).
      inline WakeTelemetry g_wakeTelemetry;

      /**
       * A notification that SystemTask would act on (the watch's notification status is On).
       *
       * The first one of a burst turns the screen on, vibrates and shows the pop-up, as it always
       * did. While the screen is still on, the next ones show as before. Once the screen has gone
       * off, a notification that comes less than kBurstWindowMs after the last notification wake is
       * only stored: the list has it, the screen stays off and the watch stays quiet. The window is
       * measured from the wake, not from the last notification, so a steady trickle wakes the
       * screen once a minute and no less often.
       */
      struct NotifyVerdict {
        bool announce;  // post System::Messages::OnNewNotification (the pop-up, the buzz, the wake)
        bool wake;      // that announcement turns the screen on
      };

      class NotifyWakeGate {
       public:
        static constexpr uint32_t kBurstWindowMs = 60000;

        NotifyVerdict Decide(uint32_t nowMs, bool screenOff) {
          if (!screenOff) return {true, false};
          if (haveWake_ && static_cast<uint32_t>(nowMs - lastWakeMs_) < kBurstWindowMs) return {false, false};
          haveWake_ = true;
          lastWakeMs_ = nowMs;
          return {true, true};
        }

       private:
        bool haveWake_ {false};
        uint32_t lastWakeMs_ {0};
      };

      /**
       * Ends a screen-on that only a notification caused, kSleepAfterPopupMs after the pop-up
       * closed. Armed when the gate lets a notification wake the screen; polled twice a second
       * until it stops. What it sees on each poll:
       *
       *  - the screen is already off: nothing to do.
       *  - the display's last activity moved since the first poll: the user touched or pressed the
       *    watch (LVGL counts both), so the normal screen timeout applies and the watch stands down.
       *  - the system is not allowed to sleep: the pop-up (it holds a wake lock while it shows,
       *    7 s) or something else is up; a lock taken again restarts the count.
       *  - otherwise the pop-up is gone: after kSleepAfterPopupMs of that, ask for sleep.
       *
       * The first poll fixes what "no activity" means (the display's own wake counts as activity,
       * and it lands a moment after the wake). If that poll comes before the display has handled the
       * wake, the wake's activity looks like the user's on the next one and the watch stands down:
       * the normal timeout applies, which is the safe side. A lock held for more than kMaxArmedMs
       * also ends the watch.
       *
       * Suspend ends a watch that is running and refuses to arm another for a while. Find Watch uses
       * it for the length of its alert (the buzzing needs the screen on), and a charging event uses
       * it for kChargingWindowMs (a plug or unplug that finds the screen on is not to be cut short).
       */
      class NotifyWakeSleepWatch {
       public:
        enum class Verdict : uint8_t { Keep, Stop, Sleep };

        static constexpr uint32_t kPollMs = 500;
        static constexpr uint32_t kSleepAfterPopupMs = 5000;
        static constexpr uint32_t kMaxArmedMs = 30000;
        static constexpr uint32_t kActivityToleranceMs = 250;
        // How long a charging event keeps the watch from arming (SystemTask, OnChargingEvent): more than
        // kSleepAfterPopupMs, so a notification wake whose Arm on the BLE task lands just after the event
        // does not put the charging screen to sleep early; the screen then times out as usual.
        static constexpr uint32_t kChargingWindowMs = 10000;

        void Arm(uint32_t nowMs) {
          if (Suspended(nowMs)) return;
          armed_ = true;
          armedAt_ = nowMs;
          haveBaseline_ = false;
          baselineActivityMs_ = 0;
          popupClosed_ = false;
          closedAt_ = 0;
        }

        bool Armed() const { return armed_; }

        // Stands the watch down now, and keeps Arm from starting another for the next forMs
        // milliseconds (0: only stands down). A longer suspension is never shortened by a later one.
        void Suspend(uint32_t nowMs, uint32_t forMs) {
          armed_ = false;
          if (forMs == 0) return;
          const uint32_t until = nowMs + forMs;
          if (!Suspended(nowMs) || static_cast<int32_t>(until - suspendUntilMs_) > 0) {
            suspended_ = true;
            suspendUntilMs_ = until;
          }
        }

        // True while a suspension runs. Looking at an expired one clears it: the end is a time modulo
        // 2^32, and a flag left set would read as running again 2^31 ms (24.8 days) after it ended.
        // Poll looks every kPollMs until then (the service keeps its timer going), so the flag never
        // outlives its end by more than one poll.
        bool Suspended(uint32_t nowMs) {
          if (!suspended_) return false;
          if (static_cast<int32_t>(suspendUntilMs_ - nowMs) > 0) return true;
          suspended_ = false;
          return false;
        }

        // idleMs: the display's time since its last activity (lv_disp_get_inactive_time).
        // sleepDisabled: a wake lock is held (SystemTask::IsSleepDisabled).
        Verdict Poll(uint32_t nowMs, bool screenOff, uint32_t idleMs, bool sleepDisabled) {
          if (!armed_) return Suspended(nowMs) ? Verdict::Keep : Verdict::Stop;  // Keep: clears it when it ends
          if (screenOff || static_cast<uint32_t>(nowMs - armedAt_) > kMaxArmedMs) {
            armed_ = false;
            return Verdict::Stop;
          }
          const uint32_t lastActivityMs = nowMs - idleMs;
          if (!haveBaseline_) {
            haveBaseline_ = true;
            baselineActivityMs_ = lastActivityMs;
          } else if (static_cast<int32_t>(lastActivityMs - baselineActivityMs_) > static_cast<int32_t>(kActivityToleranceMs)) {
            armed_ = false;
            return Verdict::Stop;
          }
          if (sleepDisabled) {
            popupClosed_ = false;
            return Verdict::Keep;
          }
          if (!popupClosed_) {
            popupClosed_ = true;
            closedAt_ = nowMs;
            return Verdict::Keep;
          }
          if (static_cast<uint32_t>(nowMs - closedAt_) >= kSleepAfterPopupMs) {
            armed_ = false;
            return Verdict::Sleep;
          }
          return Verdict::Keep;
        }

       private:
        bool armed_ {false};
        bool suspended_ {false};
        uint32_t suspendUntilMs_ {0};
        uint32_t armedAt_ {0};
        bool haveBaseline_ {false};
        uint32_t baselineActivityMs_ {0};
        bool popupClosed_ {false};
        uint32_t closedAt_ {0};
      };

      /**
       * The settings that drive the battery, packed into one number for the HELLO (WakeDiag.h):
       *   bits 0-4    wake-up modes: bit 0 single tap, 1 double tap, 2 raise wrist, 3 shake, 4 lower wrist
       *   bit 5       Always On display (the setting, whatever the notification status)
       *   bits 6-7    notification status: 0 On, 1 Off, 2 Sleep
       *   bits 8-10   brightness: 0 Off, 1 AlwaysOn, 2 Low, 3 Medium, 4 High, 5 B (High after a
       *               side-button wake, Low after any other; firmware 2.4.105)
       *   bits 11-15  screen timeout in whole seconds, 31 at most
       * The result is below 65536, so it is at most 3 bytes as a CBOR unsigned integer.
       */
      struct WakeSettings {
        uint8_t wakeModes {0};
        bool alwaysOn {false};
        uint8_t notifyStatus {0};
        uint8_t brightness {0};
        uint32_t timeoutMs {0};
      };

      inline uint32_t PackWakeSettings(const WakeSettings& s) {
        uint32_t secs = (s.timeoutMs + 500u) / 1000u;
        if (secs > 31u) secs = 31u;
        return (static_cast<uint32_t>(s.wakeModes) & 0x1Fu) | ((s.alwaysOn ? 1u : 0u) << 5) |
               ((static_cast<uint32_t>(s.notifyStatus) & 0x3u) << 6) | ((static_cast<uint32_t>(s.brightness) & 0x7u) << 8) |
               (secs << 11);
      }

      /**
       * The screen-wake telemetry of the HELLO (helloDiagRev 5, firmware 2.4.104): one optional key,
       *
       *   "wk": [wakes, notifyWakes, notifyStored, screenOnSec, settings]
       *
       * five unsigned integers, each capped at 0xFFFF (at most 3 bytes apiece) and counted since
       * boot, except `settings` (WakeTelemetry.h, PackWakeSettings). The key is 3 bytes, the array
       * header 1: at most kMaxWakeDiagBytes = 19 on the wire, about 15 in practice.
       *
       * The HELLO has no room to spare (its worst case is 241 of the 244 payload bytes the 256-byte
       * MTU carries), so BridgeController::HandleHello adds the key only when the whole map still
       * fits the frame, and sends the 15 fields it always sent when not. Absent means "not sent",
       * never zero.
       */
      struct WakeDiag {
        WakeCounters counters;
        uint32_t settings {0};
      };

      constexpr size_t kMaxWakeDiagBytes = 3 + 1 + 5 * 3;

      // The payload bytes a frame can carry at the ATT MTU `mtu` (0 when not known: the 23-byte
      // minimum, as SendSingleFrameImpl assumes): the frame buffer is 256 bytes, the link carries
      // MTU-3, and the 9-byte frame header comes off both.
      constexpr size_t kFrameHeaderBytes = 9;
      constexpr size_t kFrameBufferBytes = 256;
      inline size_t HelloPayloadRoom(uint16_t mtu) {
        const size_t effective = mtu > 0 ? mtu : 23u;
        const size_t link = effective > 3 ? effective - 3u : 0u;
        const size_t frame = link < kFrameBufferBytes ? link : kFrameBufferBytes;
        return frame > kFrameHeaderBytes ? frame - kFrameHeaderBytes : 0u;
      }

    }  // namespace HaBridge
  }    // namespace Applications
}  // namespace Pinetime
