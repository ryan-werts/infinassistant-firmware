#pragma once

// Dashboard v8 (firmware 2.4.98; docs/Plans/dashboard-v8-plan.md, docs/face_dashboard/v8/design-spec.md):
// the decisions the face's dashboard screen makes, as pure functions of the layout's view, the live
// state (DASHBOARD_STATE) and the clock. No LVGL: the face maps the results onto its objects, and
// tools/fuzz/dashboard_v8_test.cpp covers them on the host.

#include <cstddef>
#include <cstdint>

#include "ha_bridge/ActiveScreenView.h"
#include "ha_bridge/DashboardState.h"

namespace Pinetime {
  namespace Applications {
    namespace HaBridge {
      namespace DashV8 {

        // Colours, 0xRRGGBB (design-spec.md).
        constexpr uint32_t kWhite = 0xE5E5EA;
        constexpr uint32_t kGrey = 0x8E8E93;
        constexpr uint32_t kFaint = 0x48484A;
        constexpr uint32_t kRule = 0x2C2C2E;
        constexpr uint32_t kCyan = 0x5AC8FA;
        constexpr uint32_t kCheck = 0x2F7D4F;
        // The battery icon's tone while the watch has power (the plug replaces the level glyph).
        constexpr uint32_t kCharging = 0x30D158;
        constexpr uint32_t kAlarm = 0xFF453A;

        // A live state older than this greys the pills, the check and the thermostats (v7's rule).
        // The phone sends one at least every 15 min and retries a failed one each minute, so 30 min
        // means the phone or HA is out of reach, not a refresh that came a little late.
        constexpr uint32_t kStaleSec = 30u * 60u;
        // An event that started up to this long ago still shows, as "now".
        constexpr uint32_t kEventGraceSec = 10u * 60u;

        // The pill bands, in strip order: doors, garage, lights, media, motion.
        enum Band : uint8_t { BandDoors = 0, BandGarage, BandLights, BandMedia, BandMotion, kBands };
        constexpr uint8_t kSlots = 10;  // pill slots in the strip (slot 0 is the doors verdict)

        struct Pill {
          char symbol {'\0'};
          uint8_t band {0};
        };

        struct Strip {
          bool check {false};  // slot 0 holds the calm check; the pills then start at slot 1
          uint8_t count {0};   // pills, at most kSlots (kSlots - 1 with the check)
          Pill pills[kSlots] {};
        };

        /** The band a status group type draws in. Other types draw as media, as v7 placed them. */
        uint8_t BandOf(uint8_t groupType);

        /**
         * The status strip: the lit glyphs packed left to right in band order (within a band, group
         * order, then glyph order). Slot 0 is the doors verdict: the check when no door or garage
         * glyph is lit and at most 9 pills are due; with 10 or more due the check gives way (the
         * strip is saturated). Only symbols '!'..'~' are drawn. The caller has matched the state's
         * fingerprint against `dash` (StatusFingerprint).
         */
        Strip PackStatus(const ActiveScreenView::DashView& dash, const DashLive& live);

        /** A pill's fill: its band's colour, grey when stale. */
        uint32_t PillFill(uint8_t band, bool stale);
        inline uint32_t CheckColour(bool stale) {
          return stale ? kFaint : kCheck;
        }
        inline uint32_t ThermostatColour(bool stale) {
          return stale ? kGrey : kWhite;
        }

        /** The live state is older than kStaleSec (a state without a time never is). */
        bool IsStale(const DashLive& live, uint32_t nowUtc);

        /**
         * The live events to show, in order: those with a start that began at most kEventGraceSec
         * ago or is still ahead. Writes their indexes into `out` and returns how many (0..2).
         */
        uint8_t UpcomingEvents(const DashLive& live, uint32_t nowUtc, uint8_t out[DashLive::kEvents]);

        /** v7's countdown: "now" once begun, "12m" under an hour, "3.5h" under 10 h, else "12h". */
        void FormatCountdown(char* out, std::size_t size, uint32_t startUtc, uint32_t nowUtc);

        /**
         * An event start: the countdown when it has begun or begins later today, else (another
         * day) the weekday and 24 h time ("Wed 9:00"). Days and times are local: `utcOffsetSec` is
         * local time minus UTC.
         */
        void FormatEventStart(char* out, std::size_t size, uint32_t startUtc, uint32_t nowUtc, int32_t utcOffsetSec);

        /**
         * The freezer bar's text: "!17°" for the warmest freezer over its threshold, else "1 freezer
         * offline" / "N freezers offline" (N = the freezer sensors with no reading). False (and `out`
         * empty) when neither: no bar. The longest, "21 freezers offline" (3 groups of 7), is 19 glyphs
         * of 12 px in the bar's jetbrains_mono_bold_20: 228 px of the bar's 240; `size` of 20 holds it.
         */
        bool FreezerText(const DashLive& live, char* out, std::size_t size);

        /** The first two thermostat readings, "73° 69°". False (and `out` empty) when there is none. */
        bool ThermostatText(const DashLive& live, char* out, std::size_t size);

        /** The beacons at home. False when the state has no beacon reading at all. */
        bool PeopleHome(const DashLive& live, uint8_t& home);

        /** The Font Awesome battery glyph (UTF-8) for a charge level: f240 full .. f244 empty. */
        const char* BatteryIcon(uint8_t percent);

        /**
         * A title cut to fit `maxWidth`: the title itself when it fits, else its longest prefix that
         * fits with "..." appended (trailing spaces dropped), as LVGL's LONG_DOT would at one line
         * but cut at a character, not a word. Control characters become spaces; a UTF-8 sequence is
         * never split. `measure` returns the drawn width of `len` bytes of text. Returns the length.
         */
        using MeasureFn = int32_t (*)(const char* text, std::size_t len, void* ctx);
        std::size_t FitTitle(const char* title, char* out, std::size_t size, int32_t maxWidth, MeasureFn measure, void* ctx);
      }
    }
  }
}
