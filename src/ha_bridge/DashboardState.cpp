#include "ha_bridge/DashboardState.h"

#include "ha_bridge/LayoutDecoder.h"  // SkipUnknownEntry, DecodeStringInto

// DASHBOARD_STATE decode (firmware 2.4.96): a CBOR map with short keys,
//   f  fingerprint                 t  computed-at (epoch seconds)
//   g  [{t: type, b: bits}]        status groups, in layout order
//   s  [{t: type, v: [int|null], a: alert bits}]   sensor groups
//   w  {c: code, t: temp, h: hi, l: lo}            weather (h and l optional)
//   e  [{s: start epoch, n: title}]                up to 2 upcoming events
// Inline and allocation-free, as the other decoders: depth map -> array -> map -> array.

namespace Pinetime {
  namespace Applications {
    namespace HaBridge {

      namespace {
        bool KeyIs(CborValue* it, const char* key) {
          bool match = false;
          cbor_value_text_string_equals(it, key, &match);
          return match;
        }

        // An integer (or, with `allowNull`, null) and the iterator past it. False on any other
        // type, which is skipped.
        bool ReadInt(CborValue* it, int64_t& value, bool& isNull, bool allowNull) {
          isNull = false;
          if (cbor_value_is_integer(it)) {
            if (cbor_value_get_int64(it, &value) != CborNoError) {
              // Out of int64 range (a huge unsigned): saturate, as the clamps below would.
              value = INT64_MAX;
            }
            return cbor_value_advance(it) == CborNoError;
          }
          if (allowNull && cbor_value_is_null(it)) {
            isNull = true;
            return cbor_value_advance(it) == CborNoError;
          }
          return false;
        }

        int64_t Clamp(int64_t v, int64_t lo, int64_t hi) {
          return v < lo ? lo : (v > hi ? hi : v);
        }

        // Moves past the value of a map entry the decoder does not read. False for an iterator a
        // failed advance has left invalid (cut-short input), which tinycbor must not advance.
        bool SkipValue(CborValue* it) {
          return cbor_value_is_valid(it) && cbor_value_advance(it) == CborNoError;
        }

        // Each list element is a map: enter it, hand every key to `onKey` (which reads the
        // value and returns false on a malformed one), leave it. Elements beyond `cap` are
        // skipped whole.
        template <typename OnKey>
        bool ForEachElement(CborValue* list, uint8_t cap, uint8_t& count, OnKey onKey) {
          if (!cbor_value_is_array(list)) return SkipValue(list);
          CborValue element;
          if (cbor_value_enter_container(list, &element) != CborNoError) return false;
          while (!cbor_value_at_end(&element)) {
            if (count >= cap || !cbor_value_is_map(&element)) {
              if (!SkipValue(&element)) return false;
              continue;
            }
            CborValue entry;
            if (cbor_value_enter_container(&element, &entry) != CborNoError) return false;
            while (!cbor_value_at_end(&entry)) {
              if (!cbor_value_is_text_string(&entry)) {
                if (!SkipUnknownEntry(&entry)) return false;
                continue;
              }
              CborValue key = entry;
              if (cbor_value_advance(&entry) != CborNoError || cbor_value_at_end(&entry)) return false;
              if (!onKey(&key, &entry, count)) return false;
            }
            if (cbor_value_leave_container(&element, &entry) != CborNoError) return false;
            count++;
          }
          return cbor_value_leave_container(list, &element) == CborNoError;
        }
      }

      bool DecodeDashboardState(const uint8_t* payload, std::size_t len, DashLive& out) {
        out = DashLive {};
        if (payload == nullptr || len == 0) return false;

        CborParser parser;
        CborValue root;
        CborValue map;
        if (cbor_parser_init(payload, len, 0, &parser, &root) != CborNoError || !cbor_value_is_map(&root) ||
            cbor_value_enter_container(&root, &map) != CborNoError) {
          return false;
        }

        while (!cbor_value_at_end(&map)) {
          if (!cbor_value_is_text_string(&map)) {
            if (!SkipUnknownEntry(&map)) return false;
            continue;
          }
          CborValue key = map;
          if (cbor_value_advance(&map) != CborNoError || cbor_value_at_end(&map)) return false;
          int64_t v = 0;
          bool isNull = false;

          if (KeyIs(&key, "f") || KeyIs(&key, "t")) {
            if (!ReadInt(&map, v, isNull, false)) return false;
            const uint32_t u = static_cast<uint32_t>(Clamp(v, 0, UINT32_MAX));
            if (KeyIs(&key, "f")) {
              out.fingerprint = u;
            } else {
              out.computedAt = u;
            }
          } else if (KeyIs(&key, "g")) {
            const bool ok = ForEachElement(&map, DashLive::kGroups, out.groupCount,
                                           [&out](CborValue* k, CborValue* value, uint8_t i) {
              int64_t n = 0;
              bool none = false;
              if (KeyIs(k, "t")) {
                if (!ReadInt(value, n, none, false)) return false;
                out.groupType[i] = static_cast<uint8_t>(Clamp(n, 0, 255));
                return true;
              }
              if (KeyIs(k, "b")) {
                if (!ReadInt(value, n, none, false)) return false;
                out.groupBits[i] = static_cast<uint16_t>(Clamp(n, 0, UINT16_MAX));
                return true;
              }
              return SkipValue(value);
            });
            if (!ok) return false;
          } else if (KeyIs(&key, "s")) {
            const bool ok = ForEachElement(&map, DashLive::kSensorGroups, out.sensorGroupCount,
                                           [&out](CborValue* k, CborValue* value, uint8_t i) {
              int64_t n = 0;
              bool none = false;
              if (KeyIs(k, "t")) {
                if (!ReadInt(value, n, none, false)) return false;
                out.sensorType[i] = static_cast<uint8_t>(Clamp(n, 0, 255));
                return true;
              }
              if (KeyIs(k, "a")) {
                if (!ReadInt(value, n, none, false)) return false;
                out.sensorAlert[i] = static_cast<uint8_t>(Clamp(n, 0, 255));
                return true;
              }
              if (KeyIs(k, "v")) {
                if (!cbor_value_is_array(value)) return SkipValue(value);
                CborValue values;
                if (cbor_value_enter_container(value, &values) != CborNoError) return false;
                uint8_t count = 0;
                while (!cbor_value_at_end(&values)) {
                  if (count >= DashLive::kSensors) {
                    if (!SkipValue(&values)) return false;
                    continue;
                  }
                  if (cbor_value_is_integer(&values) || cbor_value_is_null(&values)) {
                    // A failed advance past the value: the message is cut short.
                    if (!ReadInt(&values, n, none, true)) return false;
                  } else {
                    if (!SkipValue(&values)) return false;  // not a number: no reading
                    none = true;
                  }
                  out.sensor[i][count++] = none ? DashLive::kNoReading
                                                : static_cast<int16_t>(Clamp(n, -32767, 32767));
                }
                out.sensorCount[i] = count;
                return cbor_value_leave_container(value, &values) == CborNoError;
              }
              return SkipValue(value);
            });
            if (!ok) return false;
          } else if (KeyIs(&key, "w")) {
            if (!cbor_value_is_map(&map)) {
              if (!SkipValue(&map)) return false;
              continue;
            }
            CborValue entry;
            if (cbor_value_enter_container(&map, &entry) != CborNoError) return false;
            bool haveHi = false;
            bool haveLo = false;
            while (!cbor_value_at_end(&entry)) {
              if (!cbor_value_is_text_string(&entry)) {
                if (!SkipUnknownEntry(&entry)) return false;
                continue;
              }
              CborValue k = entry;
              if (cbor_value_advance(&entry) != CborNoError || cbor_value_at_end(&entry)) return false;
              if (KeyIs(&k, "c") || KeyIs(&k, "t") || KeyIs(&k, "h") || KeyIs(&k, "l")) {
                if (!ReadInt(&entry, v, isNull, false)) return false;
                if (KeyIs(&k, "c")) {
                  out.weatherCode = static_cast<uint8_t>(Clamp(v, 0, 255));
                  out.hasWeather = true;
                } else {
                  const auto t = static_cast<int8_t>(Clamp(v, -127, 127));
                  if (KeyIs(&k, "t")) {
                    out.weatherTemp = t;
                  } else if (KeyIs(&k, "h")) {
                    out.weatherHi = t;
                    haveHi = true;
                  } else {
                    out.weatherLo = t;
                    haveLo = true;
                  }
                }
              } else if (!SkipValue(&entry)) {
                return false;
              }
            }
            out.hasHiLo = haveHi && haveLo;
            if (cbor_value_leave_container(&map, &entry) != CborNoError) return false;
          } else if (KeyIs(&key, "e")) {
            const bool ok = ForEachElement(&map, DashLive::kEvents, out.eventCount,
                                           [&out](CborValue* k, CborValue* value, uint8_t i) {
              if (KeyIs(k, "s")) {
                int64_t n = 0;
                bool none = false;
                if (!ReadInt(value, n, none, false)) return false;
                out.eventStart[i] = static_cast<uint32_t>(Clamp(n, 0, UINT32_MAX));
                return true;
              }
              if (KeyIs(k, "n")) {
                // A title over 24 bytes is refused (left empty), not cut: the phone cuts it.
                DecodeStringInto(value, out.eventTitle[i].data(), out.eventTitle[i].size());
                return true;
              }
              return SkipValue(value);
            });
            if (!ok) return false;
          } else if (!SkipValue(&map)) {
            return false;
          }
        }
        return cbor_value_leave_container(&root, &map) == CborNoError;
      }

    }
  }
}
