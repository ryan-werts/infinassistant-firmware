#pragma once

#include <cbor.h>
#include <cstddef>
#include <cstdint>

namespace Pinetime {
  namespace Applications {
    namespace HaBridge {

      /**
       * A SENSOR_REPORT (0x0C, watch -> phone), the values BridgeController::SendSensorReport
       * gathers every report interval. The encoding lives here, header-only, so the host tests
       * (tools/fuzz/sensor_report_test.cpp) check the bytes the watch sends, not a copy of them.
       */
      struct SensorReport {
        uint32_t steps {0};
        uint8_t heartRate {0};
        uint8_t heartRateState {0};  // 0=Stopped, 1=NotEnough, 2=NoTouch, 3=Running
        uint8_t batteryPercent {0};
        bool batteryCharging {false};
        int16_t motionX {0};
        int16_t motionY {0};
        int16_t motionZ {0};
        uint64_t timestamp {0};
        uint32_t freeHeap {0};
        uint32_t minFreeHeap {0};
        uint32_t stackHighWater {0};
        // REQ-056 — LVGL pool diagnostics.
        uint32_t lvFreeSize {0};
        uint32_t lvFreeBiggest {0};
        uint8_t lvFragPct {0};
        uint16_t lvCreateFailures {0};
        // DASHBOARD_STATE health since boot (HomeAssistantService): whether states arrive, are
        // refused, or are drawn against a layout they were not computed for. 16-bit counts wrap.
        uint16_t dashStateOk {0};        // dsOk:  decoded and published
        uint16_t dashStateRefused {0};   // dsBad: refused by DecodeDashboardState
        uint16_t fingerprintMisses {0};  // fpMis: states drawn against a layout of another fingerprint
        uint32_t dashStateAgeSec {0};    // dsAge: seconds since the last published state, 0 when none
        // bmv: battery millivolts at the last measurement (every 10 min); 0 before the first, and then the
        // key is left out, so the phone never reads a missing measurement as a flat battery.
        uint16_t batteryMv {0};
        // pp: a power source is connected, charging or not (a full battery on the charger is not
        // `bc`). Only sent when true, so an absent key is "no power" and older readers see no change.
        bool powerPresent {false};
      };

      // Entries in the map that are always there. The phone reads the keys it knows and skips the rest
      // (the LVGL four, and the dashboard four on older apps). `bmv` comes after them and only when the
      // battery has been measured, then `pp` and only while power is present, so a report holds
      // kSensorReportFields to kSensorReportFields + 2.
      constexpr std::size_t kSensorReportFields = 20;

      // The longest encoding: every value at its widest, `bmv` and `pp` included (152 B without them,
      // plus bmv's 4-byte key and 3-byte value and pp's 3-byte key and 1-byte true).
      // SendSensorReport's buffer holds it with room to spare.
      constexpr std::size_t kSensorReportMaxBytes = 163;

      /**
       * Encodes `r` as the SENSOR_REPORT map, short keys in this order:
       *   s hr hs b bc mx my mz ts   sensors and the report time
       *   fh mh sw                   free heap, minimum free heap, stack high-water (words)
       *   lvfs lvfb lvfp lvcf        LVGL free, biggest free, fragmentation %, create failures
       *   dsOk dsBad fpMis dsAge     DASHBOARD_STATE health
       *   bmv                        battery millivolts, only when measured (2.4.105)
       *   pp                         true, only while a power source is connected (2.4.105)
       * The length, or 0 when the map does not fit in `size` bytes (kSensorReportMaxBytes at most: the
       * 256-byte buffer of SendSensorReport holds any report).
       */
      inline std::size_t EncodeSensorReport(const SensorReport& r, uint8_t* buf, std::size_t size) {
        CborEncoder encoder;
        CborEncoder map;
        cbor_encoder_init(&encoder, buf, size, 0);
        const std::size_t fields = kSensorReportFields + (r.batteryMv != 0 ? 1 : 0) + (r.powerPresent ? 1 : 0);
        if (cbor_encoder_create_map(&encoder, &map, fields) != CborNoError) return 0;

        const auto key = [&map](const char* k) { return cbor_encode_text_stringz(&map, k) == CborNoError; };
        const auto uns = [&map](uint64_t v) { return cbor_encode_uint(&map, v) == CborNoError; };
        const auto sig = [&map](int64_t v) { return cbor_encode_int(&map, v) == CborNoError; };

        const bool ok = key("s") && uns(r.steps) &&
                        key("hr") && uns(r.heartRate) &&
                        key("hs") && uns(r.heartRateState) &&
                        key("b") && uns(r.batteryPercent) &&
                        key("bc") && cbor_encode_boolean(&map, r.batteryCharging) == CborNoError &&
                        key("mx") && sig(r.motionX) &&
                        key("my") && sig(r.motionY) &&
                        key("mz") && sig(r.motionZ) &&
                        key("ts") && uns(r.timestamp) &&
                        key("fh") && uns(r.freeHeap) &&
                        key("mh") && uns(r.minFreeHeap) &&
                        key("sw") && uns(r.stackHighWater) &&
                        key("lvfs") && uns(r.lvFreeSize) &&
                        key("lvfb") && uns(r.lvFreeBiggest) &&
                        key("lvfp") && uns(r.lvFragPct) &&
                        key("lvcf") && uns(r.lvCreateFailures) &&
                        key("dsOk") && uns(r.dashStateOk) &&
                        key("dsBad") && uns(r.dashStateRefused) &&
                        key("fpMis") && uns(r.fingerprintMisses) &&
                        key("dsAge") && uns(r.dashStateAgeSec) &&
                        (r.batteryMv == 0 || (key("bmv") && uns(r.batteryMv))) &&
                        (!r.powerPresent || (key("pp") && cbor_encode_boolean(&map, true) == CborNoError));
        if (!ok || cbor_encoder_close_container(&encoder, &map) != CborNoError) return 0;
        return cbor_encoder_get_buffer_size(&encoder, buf);
      }

    }
  }
}
