#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace Pinetime {
  namespace Applications {
    namespace HaBridge {

      /**
       * The dashboard's live state (DASHBOARD_STATE, 0x20, firmware 2.4.96): what the phone
       * recomputes whenever a dashboard entity changes in Home Assistant. It is kept in RAM
       * and never written to flash. Until 2.4.96 these values came only inside a layout push,
       * which happens only when the layout is pushed from the card or the app: the status
       * letters, temperatures, weather and meeting rows stayed as they were at that push.
       * The wire format is in docs/reference/ble-protocol-spec.md (0x20).
       */
      struct DashLive {
        static constexpr uint8_t kGroups = 5;        // HomeAssistantService::kMaxStatusGroups
        static constexpr uint8_t kSensorGroups = 3;  // HomeAssistantService::kMaxSensorGroups
        static constexpr uint8_t kSensors = 7;       // values in a sensor group
        static constexpr uint8_t kEvents = 2;
        static constexpr int16_t kNoReading = INT16_MIN;
        static constexpr std::size_t kTitleSize = 25;  // 24 bytes and the end

        // Status group types, as the phone numbers them.
        enum GroupType : uint8_t { Other = 0, Doors = 1, Garage = 2, Motion = 3, Lights = 4, Media = 5 };
        // Sensor group types.
        enum SensorType : uint8_t { SensorOther = 0, Thermostats = 1, Freezers = 2, Beacons = 3 };

        uint32_t fingerprint {0};  // StatusFingerprint of the layout the bits were computed for
        uint32_t computedAt {0};   // the phone's UTC epoch seconds
        uint8_t groupCount {0};
        std::array<uint8_t, kGroups> groupType {};
        std::array<uint16_t, kGroups> groupBits {};  // bit i: glyph i of the group is active
        uint8_t sensorGroupCount {0};
        std::array<uint8_t, kSensorGroups> sensorType {};
        std::array<uint8_t, kSensorGroups> sensorCount {};
        std::array<uint8_t, kSensorGroups> sensorAlert {};  // bit i: sensor i is over its threshold
        std::array<std::array<int16_t, kSensors>, kSensorGroups> sensor {};
        bool hasWeather {false};
        bool hasHiLo {false};
        uint8_t weatherCode {0};  // WeatherCode
        int8_t weatherTemp {0};
        int8_t weatherHi {0};
        int8_t weatherLo {0};
        uint8_t eventCount {0};
        std::array<uint32_t, kEvents> eventStart {};  // UTC epoch seconds
        std::array<std::array<char, kTitleSize>, kEvents> eventTitle {};
      };

      /** HA's weather conditions, as the phone sends them (one byte instead of the string). */
      enum WeatherCode : uint8_t {
        WxNone = 0,
        WxSunny,
        WxClearNight,
        WxPartlyCloudy,
        WxCloudy,
        WxRainy,
        WxPouring,
        WxSnowy,
        WxSnowyRainy,
        WxHail,
        WxLightning,
        WxLightningRainy,
        WxWindy,
        WxWindyVariant,
        WxFog,
        WxExceptional,
      };

      /**
       * Decodes a DASHBOARD_STATE payload into `out`, which is reset first. False on input
       * that is not a map or is cut short: the caller publishes nothing then, so a malformed
       * message never leaves half a state to draw. Unknown keys are skipped; lists longer
       * than the caps above are cut; out-of-range numbers are clamped.
       */
      bool DecodeDashboardState(const uint8_t* payload, std::size_t len, DashLive& out);

      /**
       * The status fingerprint is FNV-1a over, for each status group in order, its glyph
       * count (one byte) and then its glyph symbols (one byte each). The phone hashes the
       * same bytes of the layout it pushed; a DASHBOARD_STATE's bits are used only against a
       * layout with the same fingerprint (a state computed for one layout must not light
       * the letters of another).
       */
      constexpr uint32_t kFnvOffset = 2166136261u;
      constexpr uint32_t FnvAdd(uint32_t hash, uint8_t byte) {
        return (hash ^ byte) * 16777619u;
      }

    }
  }
}
