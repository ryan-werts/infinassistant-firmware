#include "ha_bridge/DashboardV8.h"

#include <cstdio>
#include <cstring>

namespace Pinetime {
  namespace Applications {
    namespace HaBridge {
      namespace DashV8 {

        namespace {
          constexpr const char* kDegree = "\xC2\xB0";
          constexpr uint32_t kBandColour[kBands] = {0xFF9500 /* doors */, 0xFF453A /* garage */, 0xFFCC00 /* lights */,
                                                    0xBF5AF2 /* media */, 0x5AC8FA /* motion */};
          constexpr const char* kWeekday[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        }

        uint8_t BandOf(uint8_t groupType) {
          switch (groupType) {
            case DashLive::Doors:
              return BandDoors;
            case DashLive::Garage:
              return BandGarage;
            case DashLive::Lights:
              return BandLights;
            case DashLive::Motion:
              return BandMotion;
            default:
              return BandMedia;
          }
        }

        Strip PackStatus(const ActiveScreenView::DashView& dash, const DashLive& live) {
          Strip strip;
          uint8_t groups = live.groupCount < dash.statusGroupCount ? live.groupCount : dash.statusGroupCount;
          if (groups > DashLive::kGroups) groups = DashLive::kGroups;
          if (groups > dash.statusGroups.size()) groups = static_cast<uint8_t>(dash.statusGroups.size());

          bool doorsLit = false;
          uint8_t due = 0;  // lit glyphs, counted past kSlots for the saturation rule
          for (uint8_t band = 0; band < kBands; band++) {
            for (uint8_t g = 0; g < groups; g++) {
              if (BandOf(live.groupType[g]) != band) continue;
              const auto& grp = dash.statusGroups[g];
              uint8_t count = grp.glyphCount < grp.glyphs.size() ? grp.glyphCount : static_cast<uint8_t>(grp.glyphs.size());
              if (count > 16) count = 16;
              for (uint8_t i = 0; i < count; i++) {
                const char symbol = grp.glyphs[i].symbol;
                if ((live.groupBits[g] & (1u << i)) == 0 || symbol < '!' || symbol > '~') continue;
                if (band == BandDoors || band == BandGarage) doorsLit = true;
                if (due < kSlots) strip.pills[due] = Pill {symbol, band};
                if (due < 0xFF) due++;
              }
            }
          }
          strip.check = !doorsLit && due < kSlots;
          strip.count = due < kSlots ? due : kSlots;
          return strip;
        }

        uint32_t PillFill(uint8_t band, bool stale) {
          if (stale) return kGrey;
          return band < kBands ? kBandColour[band] : kBandColour[BandMedia];
        }

        bool IsStale(const DashLive& live, uint32_t nowUtc) {
          return live.computedAt != 0 && nowUtc > live.computedAt + kStaleSec;
        }

        uint8_t UpcomingEvents(const DashLive& live, uint32_t nowUtc, uint8_t out[DashLive::kEvents]) {
          uint8_t n = 0;
          for (uint8_t i = 0; i < live.eventCount && i < DashLive::kEvents; i++) {
            const uint32_t start = live.eventStart[i];
            if (start != 0 && static_cast<uint64_t>(start) + kEventGraceSec > nowUtc) out[n++] = i;
          }
          return n;
        }

        void FormatCountdown(char* out, std::size_t size, uint32_t startUtc, uint32_t nowUtc) {
          if (startUtc <= nowUtc) {
            snprintf(out, size, "now");
            return;
          }
          const uint32_t mins = (startUtc - nowUtc) / 60u;
          if (mins < 60u) {
            snprintf(out, size, "%um", static_cast<unsigned>(mins));
            return;
          }
          const uint32_t tenths = (mins * 10u) / 60u;  // hours x10; exactly 60 min is 1.0h
          if (tenths < 100u) {
            snprintf(out, size, "%u.%uh", static_cast<unsigned>(tenths / 10u), static_cast<unsigned>(tenths % 10u));
          } else {
            snprintf(out, size, "%uh", static_cast<unsigned>(tenths / 10u));
          }
        }

        void FormatEventStart(char* out, std::size_t size, uint32_t startUtc, uint32_t nowUtc, int32_t utcOffsetSec) {
          const auto localDay = [utcOffsetSec](uint32_t utc) {
            const int64_t local = static_cast<int64_t>(utc) + utcOffsetSec;
            return local >= 0 ? local / 86400 : (local - 86399) / 86400;
          };
          const int64_t days = localDay(startUtc);
          if (startUtc <= nowUtc || days == localDay(nowUtc)) {
            FormatCountdown(out, size, startUtc, nowUtc);
            return;
          }
          const int64_t secOfDay = static_cast<int64_t>(startUtc) + utcOffsetSec - days * 86400;
          const int weekday = static_cast<int>(((days % 7) + 7 + 4) % 7);  // 1970-01-01 was a Thursday
          snprintf(out, size, "%s %d:%02d", kWeekday[weekday], static_cast<int>(secOfDay / 3600),
                   static_cast<int>((secOfDay % 3600) / 60));
        }

        bool FreezerText(const DashLive& live, char* out, std::size_t size) {
          bool alert = false;
          unsigned dead = 0;  // freezer sensors with no reading
          int warmest = INT16_MIN;
          for (uint8_t g = 0; g < live.sensorGroupCount && g < DashLive::kSensorGroups; g++) {
            if (live.sensorType[g] != DashLive::Freezers) continue;
            for (uint8_t i = 0; i < live.sensorCount[g] && i < DashLive::kSensors; i++) {
              const int16_t v = live.sensor[g][i];
              if (v == DashLive::kNoReading) {
                dead++;
              } else if ((live.sensorAlert[g] & (1u << i)) != 0) {
                alert = true;
                if (v > warmest) warmest = v;
              }
            }
          }
          if (size == 0) return false;
          out[0] = '\0';
          if (alert) {
            snprintf(out, size, "!%d%s", warmest, kDegree);
          } else if (dead != 0) {
            snprintf(out, size, dead == 1 ? "1 freezer offline" : "%u freezers offline", dead);
          }
          return out[0] != '\0';
        }

        bool ThermostatText(const DashLive& live, char* out, std::size_t size) {
          if (size == 0) return false;
          out[0] = '\0';
          std::size_t used = 0;
          uint8_t shown = 0;
          for (uint8_t g = 0; g < live.sensorGroupCount && g < DashLive::kSensorGroups && shown < 2; g++) {
            if (live.sensorType[g] != DashLive::Thermostats) continue;
            for (uint8_t i = 0; i < live.sensorCount[g] && i < DashLive::kSensors && shown < 2; i++) {
              const int16_t v = live.sensor[g][i];
              if (v == DashLive::kNoReading) continue;
              const int n = snprintf(out + used, size - used, shown == 0 ? "%d%s" : " %d%s", v, kDegree);
              if (n < 0 || used + static_cast<std::size_t>(n) >= size) {
                out[used] = '\0';  // no half reading
                return shown != 0;
              }
              used += static_cast<std::size_t>(n);
              shown++;
            }
          }
          return shown != 0;
        }

        bool PeopleHome(const DashLive& live, uint8_t& home) {
          bool any = false;
          home = 0;
          for (uint8_t g = 0; g < live.sensorGroupCount && g < DashLive::kSensorGroups; g++) {
            if (live.sensorType[g] != DashLive::Beacons) continue;
            for (uint8_t i = 0; i < live.sensorCount[g] && i < DashLive::kSensors; i++) {
              const int16_t v = live.sensor[g][i];
              if (v == DashLive::kNoReading) continue;
              any = true;
              if (v > 0 && home < 0xFF) home++;
            }
          }
          return any;
        }

        const char* BatteryIcon(uint8_t percent) {
          if (percent >= 90) return "\xEF\x89\x80";  // f240
          if (percent >= 60) return "\xEF\x89\x81";  // f241
          if (percent >= 35) return "\xEF\x89\x82";  // f242
          if (percent >= 10) return "\xEF\x89\x83";  // f243
          return "\xEF\x89\x84";                      // f244
        }

        std::size_t FitTitle(const char* title, char* out, std::size_t size, int32_t maxWidth, MeasureFn measure, void* ctx) {
          if (out == nullptr || size == 0) return 0;
          out[0] = '\0';
          if (title == nullptr || size < 5) return 0;
          // The title, control characters as spaces, cut at a whole UTF-8 sequence that leaves room
          // for "..." and the end.
          std::size_t len = 0;
          for (const char* c = title; *c != '\0' && len + 4 < size; c++) {
            out[len++] = (static_cast<unsigned char>(*c) < 0x20) ? ' ' : *c;
          }
          while (len > 0 && (static_cast<unsigned char>(title[len]) & 0xC0) == 0x80) len--;
          const bool whole = title[len] == '\0';
          out[len] = '\0';
          if (whole && measure(out, len, ctx) <= maxWidth) return len;

          // Longest prefix with "..." that fits.
          for (std::size_t n = len; n > 0; n--) {
            if ((static_cast<unsigned char>(out[n]) & 0xC0) == 0x80 && n < len) continue;  // inside a sequence
            std::size_t k = n;
            while (k > 0 && out[k - 1] == ' ') k--;
            if (k == 0) break;
            char saved[4];
            std::memcpy(saved, out + k, 4);
            std::memcpy(out + k, "...", 4);
            if (measure(out, k + 3, ctx) <= maxWidth) return k + 3;
            std::memcpy(out + k, saved, 4);
          }
          std::memcpy(out, "...", 4);
          return 3;
        }
      }
    }
  }
}
