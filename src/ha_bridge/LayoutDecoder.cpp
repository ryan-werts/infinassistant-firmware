#include "ha_bridge/LayoutDecoder.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <tuple>

#include "DashboardLimits.h"

namespace Pinetime {
namespace Applications {
namespace HaBridge {

namespace {

// Deepest container nesting SkipItem follows: the limit the handler's pre-scan
// (CborNestingOk) enforces on every payload, and the structural pre-pass in
// DecodeLayoutDocument enforces again by running this same skip.
constexpr uint8_t kSkipDepth = DashboardLimits::MAX_CBOR_DEPTH;

// The capacities the array loops stop at, taken from the target arrays
// themselves: a loop cap that outgrew its array would write past it.
constexpr size_t kEventSlots = std::tuple_size<decltype(LayoutService::Dashboard::events)>::value;
constexpr size_t kSensorGroupSlots = std::tuple_size<decltype(LayoutService::Dashboard::sensorGroups)>::value;
constexpr size_t kSensorValueSlots = std::tuple_size<decltype(LayoutService::SensorGroup::values)>::value;
// The validate-mode shadow keeps one value counter per sensor group.
static_assert(sizeof(DashShadow::valueCount) / sizeof(DashShadow::valueCount[0]) == kSensorGroupSlots,
              "DashShadow::valueCount must have one entry per sensor group");

// A pointer to the array member `p->f`, or nullptr when there is no target. The
// pointer is what DecodeStringField needs: it knows the capacity from the array
// type even when no target exists, so a no-write decode applies the same
// length check a write does.
#define HA_FIELD(p, f) ((p) ? &(p)->f : nullptr)

template <size_t N>
bool DecodeStringField(CborValue* value, std::array<char, N>* field) {
  return DecodeStringInto(value, field ? field->data() : nullptr, N);
}

// Advance past a scalar the walk has just looked at. A failed advance leaves
// the iterator inconsistent and the next at_end() then reports false — see
// SkipUnknownEntry — so the caller must return on failure.
inline bool SkipValueChecked(CborValue* it) {
  return cbor_value_at_end(it) || SkipItem(it) == CborNoError;
}

// Every caller of DecodeStringInto treats false as "this value was not stored"
// and carries on, so a failed skip or copy cannot be reported through the
// return value. Mark the iterator instead: SkipItem refuses an invalid iterator,
// so the walk's next skip fails and the decode returns.
inline void Poison(CborValue* it) { it->type = CborInvalidType; }

}  // namespace

// Skip exactly one data item without recursion. See LayoutDecoder.h.
//
// `stack[k]` walks the children of the container entered at nesting level k;
// its parent (the iterator cbor_value_leave_container needs) is `stack[k-1]`,
// or `it` for k == 0. Every step works on those copies, so `it` itself is only
// written by the final leave (or by the scalar advance when `it` is a scalar):
// the same points at which cbor_value_advance() writes it, and so the same
// iterator state after a failure.
//
// Not inlined: it is called from every skip site, and its frame (one CborValue
// per level) must sit at the bottom of the chain once, not be duplicated into
// each decoder's frame.
__attribute__((noinline)) CborError SkipItem(CborValue* it) {
  if (cbor_value_at_end(it)) return CborErrorAdvancePastEOF;

  CborValue stack[kSkipDepth];
  uint8_t depth = 0;
  for (;;) {
    CborValue* cur = depth == 0 ? it : &stack[depth - 1];
    CborError err;

    if (depth > 0 && cbor_value_at_end(cur)) {
      CborValue* parent = depth == 1 ? it : &stack[depth - 2];
      err = cbor_value_leave_container(parent, cur);
      if (err != CborNoError) return err;
      if (--depth == 0) return CborNoError;
      continue;
    }

    // An iterator that is not at the end but holds no item: a container cut off
    // before its next promised item (tinycbor sets CborInvalidType there and
    // reports the EOF only from the advance that got it there). tinycbor's
    // advance asserts on it, which is undefined behaviour in a -DNDEBUG build.
    if (!cbor_value_is_valid(cur)) return CborErrorUnexpectedEOF;

    if (cbor_value_is_container(cur)) {
      if (depth == kSkipDepth) return CborErrorNestingTooDeep;
      err = cbor_value_enter_container(cur, &stack[depth]);
      if (err != CborNoError) return err;
      ++depth;
      continue;
    }

    if (cbor_value_is_text_string(cur)) {
      size_t len = SIZE_MAX;
      err = cbor_value_copy_text_string(cur, nullptr, &len, cur);
    } else if (cbor_value_is_byte_string(cur)) {
      size_t len = SIZE_MAX;
      err = cbor_value_copy_byte_string(cur, nullptr, &len, cur);
    } else {
      err = cbor_value_advance_fixed(cur);
    }
    if (err != CborNoError) return err;
    if (depth == 0) return CborNoError;
  }
}

// Skip a map entry whose key we do not recognise.
//
// The idiom this replaces discarded both return values:
//
//     cbor_value_advance(&it);
//     if (!cbor_value_at_end(&it)) cbor_value_advance(&it);
//
// When the first advance FAILS on malformed input it leaves the iterator in an
// inconsistent state; cbor_value_at_end() then reports false and the second
// advance walks off the end of the buffer. ASAN caught exactly that from the
// new fuzz_layout_screen target — a 1-byte read 53 bytes past a 77-byte
// payload, via cbor_value_advance -> preparse_value -> read_bytes.
//
// Reachable from any malformed or truncated WATCHFACE_LAYOUT, so it is worth
// checking rather than trusting the sender.
bool SkipUnknownEntry(CborValue* it) {
  if (SkipItem(it) != CborNoError) return false;  // past the key
  if (cbor_value_at_end(it)) return true;         // key with no value
  if (SkipItem(it) != CborNoError) return false;  // past the value
  return true;
}

// Decode CBOR text string directly into a fixed-size char array.
// ZERO heap allocation — no std::string intermediary.
// Always advances the CborValue iterator (prevents infinite loops on error).
// `out` == nullptr decodes without storing (no-write mode): tinycbor's copy with
// a null buffer runs the identical length and chunk checks and moves `value`
// identically, it just does not copy.
bool DecodeStringInto(CborValue* value, char* out, size_t outSize) {
  // REQ-026 — on every failure path, clear out[0] so callers cannot read stale
  // garbage. Required because REQ-018's stack-array zero-init only covers the
  // initial allocation; a partial decode that hits a failure mid-string used to
  // leave whatever bytes cbor_value_copy_text_string wrote.
  if (out != nullptr && outSize > 0) {
    out[0] = '\0';
  }
  if (!cbor_value_is_text_string(value)) {
    if (SkipItem(value) != CborNoError) Poison(value);
    return false;
  }

  size_t strLen = 0;
  if (cbor_value_calculate_string_length(value, &strLen) != CborNoError || strLen >= outSize) {
    if (SkipItem(value) != CborNoError) Poison(value);
    return false;
  }

  size_t bufLen = outSize;
  if (cbor_value_copy_text_string(value, out, &bufLen, value) != CborNoError) {
    if (out != nullptr && outSize > 0) out[0] = '\0';
    Poison(value);
    return false;
  }
  if (out != nullptr) out[bufLen] = '\0';
  return true;
}

// --- Zero-heap inline CBOR decode helpers for HandleWatchFaceLayout ---
// These decode CBOR maps/arrays directly into HomeAssistantService fixed-size structs.
// No std::string, no std::vector — zero heap allocation per call.
// WHY: WatchFaceLayoutMessage::DecodeCbor performed 100+ heap allocations for nested
// strings/vectors. On the BLE callback stack, this caused crashes from combined
// stack pressure + heap fragmentation (v1.111.0 crash patterns 1-3).
//
// Each decoder takes its target as an optional pointer (or a Ref, see the
// header): given one it fills it, given none it decodes, range-checks and
// counts identically and stores nothing. One body serves both modes.
//
// Every key is matched in its long and short spelling (KeyIsPair +
// HA_WIRE_KEY, LayoutDecoder.h). Duplicate keys follow the rule stated there:
// a repeated scalar is last-wins, a repeated array key keeps the first
// occurrence and skips the later ones. The bound on every array stays its own
// count-below-capacity loop guard, so no spelling can push past a dimension.

namespace {

bool DecodeTile(CborValue* iter, LayoutService::WatchTile* tile) {
  if (!cbor_value_is_map(iter)) return false;

  // CRITICAL: Parent struct is memset(0) which sets colorEnabled to false.
  // Default to true here so dynamic entity-state coloring works even when
  // the CBOR payload omits the "colorEnabled" field.
  if (tile) tile->colorEnabled = true;

  CborValue m;
  if (cbor_value_enter_container(iter, &m) != CborNoError) return false;

  while (!cbor_value_at_end(&m)) {
    if (!cbor_value_is_text_string(&m)) {
      if (!SkipUnknownEntry(&m)) return false;
      continue;
    }

    if (KeyIsPair(&m, HA_WIRE_KEY(TILE, "label"))) { if (!SkipKey(&m)) return false; DecodeStringField(&m, HA_FIELD(tile, label)); continue; }

    if (KeyIsPair(&m, HA_WIRE_KEY(TILE, "icon"))) { if (!SkipKey(&m)) return false; DecodeStringField(&m, HA_FIELD(tile, icon)); continue; }

    if (KeyIsPair(&m, HA_WIRE_KEY(TILE, "entityId"))) { if (!SkipKey(&m)) return false; DecodeStringField(&m, HA_FIELD(tile, entityId)); continue; }

    if (KeyIsPair(&m, HA_WIRE_KEY(TILE, "color"))) { if (!SkipKey(&m)) return false; DecodeStringField(&m, HA_FIELD(tile, color)); continue; }

    if (KeyIsPair(&m, HA_WIRE_KEY(TILE, "colorEnabled"))) {
      if (!SkipKey(&m)) return false;
      if (!cbor_value_at_end(&m) && cbor_value_is_boolean(&m)) {
        bool val = true;
        cbor_value_get_boolean(&m, &val);
        if (tile) tile->colorEnabled = val;
      }
      // Checked: a failed advance leaves the iterator inconsistent, and the
      // next at_end() then reports false — see SkipUnknownEntry.
      if (!SkipValueChecked(&m)) return false;
      continue;
    }

    if (KeyIsPair(&m, HA_WIRE_KEY(TILE, "confirm"))) {
      if (!SkipKey(&m)) return false;
      if (!cbor_value_at_end(&m) && cbor_value_is_boolean(&m)) {
        bool val = false;
        cbor_value_get_boolean(&m, &val);
        if (tile) tile->confirm = val;
      }
      // Checked: a failed advance leaves the iterator inconsistent, and the
      // next at_end() then reports false — see SkipUnknownEntry.
      if (!SkipValueChecked(&m)) return false;
      continue;
    }

    if (KeyIsPair(&m, HA_WIRE_KEY(TILE, "displayMode"))) {
      if (!SkipKey(&m)) return false;
      if (!cbor_value_at_end(&m) && cbor_value_is_integer(&m)) {
        int64_t v = 0;
        cbor_value_get_int64(&m, &v);
        // REQ-027 — clamp to uint8 range; drop wrap behaviour.
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        if (tile) tile->displayMode = static_cast<uint8_t>(v);
      }
      // Checked: a failed advance leaves the iterator inconsistent, and the
      // next at_end() then reports false — see SkipUnknownEntry.
      if (!SkipValueChecked(&m)) return false;
      continue;
    }

    if (!SkipUnknownEntry(&m)) return false;
  }

  return cbor_value_leave_container(iter, &m) == CborNoError;
}

bool DecodeEvent(CborValue* iter, LayoutService::CalendarEvent* event) {
  if (!cbor_value_is_map(iter)) return false;
  CborValue m;
  if (cbor_value_enter_container(iter, &m) != CborNoError) return false;

  while (!cbor_value_at_end(&m)) {
    if (!cbor_value_is_text_string(&m)) {
      if (!SkipUnknownEntry(&m)) return false;
      continue;
    }

    if (KeyIsPair(&m, HA_WIRE_KEY(EVENT, "time"))) { if (!SkipKey(&m)) return false; DecodeStringField(&m, HA_FIELD(event, time)); continue; }

    if (KeyIsPair(&m, HA_WIRE_KEY(EVENT, "title"))) { if (!SkipKey(&m)) return false; DecodeStringField(&m, HA_FIELD(event, title)); continue; }

    if (KeyIsPair(&m, HA_WIRE_KEY(EVENT, "ese"))) {
      if (!SkipKey(&m)) return false;
      if (!cbor_value_at_end(&m) && cbor_value_is_integer(&m)) {
        int64_t v = 0;
        cbor_value_get_int64(&m, &v);
        if (event) event->eventStartEpoch = static_cast<uint32_t>(v);
      }
      // Checked: a failed advance leaves the iterator inconsistent, and the
      // next at_end() then reports false — see SkipUnknownEntry.
      if (!SkipValueChecked(&m)) return false;
      continue;
    }

    if (!SkipUnknownEntry(&m)) return false;
  }

  return cbor_value_leave_container(iter, &m) == CborNoError;
}

// `glyphCount` is the count this group has taken so far: the real
// group->glyphCount in write mode, a shadow counter in no-write mode.
bool DecodeStatusGroup(CborValue* iter, LayoutService::StatusGroup* group, uint8_t* glyphCount) {
  if (!cbor_value_is_map(iter)) return false;
  CborValue m;
  if (cbor_value_enter_container(iter, &m) != CborNoError) return false;
  uint8_t itemsSeen = 0;  // array-key claim (duplicate rule, LayoutDecoder.h)

  while (!cbor_value_at_end(&m)) {
    if (!cbor_value_is_text_string(&m)) {
      if (!SkipUnknownEntry(&m)) return false;
      continue;
    }

    if (KeyIsPair(&m, HA_WIRE_KEY(STATUS_GROUP, "glyphs"))) {
      if (!SkipKey(&m)) return false;
      if (ArrayKeyTaken(&itemsSeen, kSeenItems)) {
        if (!SkipValueChecked(&m)) return false;
      } else if (!cbor_value_at_end(&m) && cbor_value_is_array(&m)) {
        CborValue arr;
        if (cbor_value_enter_container(&m, &arr) == CborNoError) {
          while (!cbor_value_at_end(&arr) && *glyphCount < LayoutService::kMaxGlyphsPerGroup) {
            if (cbor_value_is_map(&arr)) {
              CborValue gm;
              if (cbor_value_enter_container(&arr, &gm) == CborNoError) {
                char symbolBuf[4] = {};
                bool active = false;
                while (!cbor_value_at_end(&gm)) {
                  if (!cbor_value_is_text_string(&gm)) {
                    if (!SkipUnknownEntry(&gm)) return false;
                    continue;
                  }
                  if (KeyIsPair(&gm, HA_WIRE_KEY(GLYPH, "symbol"))) { if (!SkipKey(&gm)) return false; DecodeStringInto(&gm, symbolBuf, sizeof(symbolBuf)); continue; }
                  if (KeyIsPair(&gm, HA_WIRE_KEY(GLYPH, "active"))) {
                    if (!SkipKey(&gm)) return false;
                    if (!cbor_value_at_end(&gm) && cbor_value_is_boolean(&gm)) {
                      cbor_value_get_boolean(&gm, &active);
                    }
                    // Checked: a failed advance leaves the iterator inconsistent, and the
                    // next at_end() then reports false — see SkipUnknownEntry.
                    if (!SkipValueChecked(&gm)) return false;
                    continue;
                  }
                  if (!SkipUnknownEntry(&gm)) return false;
                }
                if (cbor_value_leave_container(&arr, &gm) != CborNoError) return false;
                // REQ-028 — glyph_symbol_storage = ascii_only (SOT:
                // scripts/config/firmware-limits.yaml). StatusGlyph::symbol is
                // a single byte and JetBrains Mono on the watch has no glyph
                // coverage above 0x7F, so silently truncating UTF-8 to byte 0
                // produced garbage rectangles. Reject any glyph whose decoded
                // symbol is not exactly one printable ASCII byte. Android-side
                // preflight (REQ-073) is the user-facing gate; this is the
                // last-line firmware-side defense.
                const uint8_t sb0 = static_cast<uint8_t>(symbolBuf[0]);
                const bool symbolIsAscii =
                    (sb0 != 0u) && (sb0 < 0x80u) && (symbolBuf[1] == '\0');
                if (symbolIsAscii) {
                  if (group) {
                    group->glyphs[*glyphCount].symbol = symbolBuf[0];
                    group->glyphs[*glyphCount].active = active;
                  }
                  ++*glyphCount;
                }
              } else {
                if (SkipItem(&arr) != CborNoError) return false;
              }
            } else {
              if (SkipItem(&arr) != CborNoError) return false;
            }
          }
          if (!SkipRest(&arr)) return false;
          if (cbor_value_leave_container(&m, &arr) != CborNoError) return false;
        }
      } else if (!cbor_value_at_end(&m)) {
        if (SkipItem(&m) != CborNoError) return false;
      }
      continue;
    }

    if (!SkipUnknownEntry(&m)) return false;
  }

  return cbor_value_leave_container(iter, &m) == CborNoError;
}

// `valueCount` as for DecodeStatusGroup's `glyphCount`.
bool DecodeSensorGroup(CborValue* iter, LayoutService::SensorGroup* group, uint8_t* valueCount) {
  if (!cbor_value_is_map(iter)) return false;
  CborValue m;
  if (cbor_value_enter_container(iter, &m) != CborNoError) return false;
  uint8_t itemsSeen = 0;  // array-key claim (duplicate rule, LayoutDecoder.h)

  while (!cbor_value_at_end(&m)) {
    if (!cbor_value_is_text_string(&m)) {
      if (!SkipUnknownEntry(&m)) return false;
      continue;
    }

    if (KeyIsPair(&m, HA_WIRE_KEY(SENSOR_GROUP, "sensors"))) {
      if (!SkipKey(&m)) return false;
      if (ArrayKeyTaken(&itemsSeen, kSeenItems)) {
        if (!SkipValueChecked(&m)) return false;
      } else if (!cbor_value_at_end(&m) && cbor_value_is_array(&m)) {
        CborValue arr;
        if (cbor_value_enter_container(&m, &arr) == CborNoError) {
          while (!cbor_value_at_end(&arr) && *valueCount < kSensorValueSlots) {
            if (cbor_value_is_map(&arr)) {
              CborValue sm;
              if (cbor_value_enter_container(&arr, &sm) == CborNoError) {
                int64_t value = 0;
                while (!cbor_value_at_end(&sm)) {
                  if (!cbor_value_is_text_string(&sm)) {
                    if (!SkipUnknownEntry(&sm)) return false;
                    continue;
                  }
                  if (KeyIsPair(&sm, HA_WIRE_KEY(SENSOR, "value"))) {
                    if (!SkipKey(&sm)) return false;
                    if (!cbor_value_at_end(&sm) && cbor_value_is_integer(&sm)) {
                      cbor_value_get_int64(&sm, &value);
                    }
                    // Checked: a failed advance leaves the iterator inconsistent, and the
                    // next at_end() then reports false — see SkipUnknownEntry.
                    if (!SkipValueChecked(&sm)) return false;
                    continue;
                  }
                  // Skip entityId, displayType — only the int value is stored
                  if (!SkipUnknownEntry(&sm)) return false;
                }
                if (cbor_value_leave_container(&arr, &sm) != CborNoError) return false;
                // REQ-027 — clamp the sensor value to int16 range so a wild
                // int64 cannot wrap to a different sign/magnitude.
                if (value < INT16_MIN) value = INT16_MIN;
                if (value > INT16_MAX) value = INT16_MAX;
                if (group) group->values[*valueCount] = static_cast<int16_t>(value);
                ++*valueCount;
              } else {
                if (SkipItem(&arr) != CborNoError) return false;
              }
            } else {
              if (SkipItem(&arr) != CborNoError) return false;
            }
          }
          if (!SkipRest(&arr)) return false;
          if (cbor_value_leave_container(&m, &arr) != CborNoError) return false;
        }
      } else if (!cbor_value_at_end(&m)) {
        if (SkipItem(&m) != CborNoError) return false;
      }
      continue;
    }

    if (!SkipUnknownEntry(&m)) return false;
  }

  return cbor_value_leave_container(iter, &m) == CborNoError;
}

// Three wire fields are not stored, because no screen renders them: the
// dashboard "notifications" array, the weather "temp", and the "type" of a
// status or sensor group. Their keys, in either spelling, reach the unknown-key
// skip below, so a payload that still carries them decodes as before.
bool DecodeDashboard(CborValue* iter, DashRef dash) {
  if (!cbor_value_is_map(iter)) return false;
  CborValue m;
  if (cbor_value_enter_container(iter, &m) != CborNoError) return false;
  uint8_t arraysSeen = 0;  // array-key claims of this dashboard map (duplicate rule, LayoutDecoder.h)

  while (!cbor_value_at_end(&m)) {
    if (!cbor_value_is_text_string(&m)) {
      if (!SkipUnknownEntry(&m)) return false;
      continue;
    }

    if (KeyIsPair(&m, HA_WIRE_KEY(DASHBOARD, "statusGroups"))) {
      if (!SkipKey(&m)) return false;
      if (ArrayKeyTaken(&arraysSeen, kSeenStatusGroups)) {
        if (!SkipValueChecked(&m)) return false;
      } else if (!cbor_value_at_end(&m) && cbor_value_is_array(&m)) {
        CborValue arr;
        if (cbor_value_enter_container(&m, &arr) == CborNoError) {
          while (!cbor_value_at_end(&arr) && dash.statusGroupCount() < LayoutService::kMaxStatusGroups) {
            const uint8_t g = dash.statusGroupCount();
            if (DecodeStatusGroup(&arr, dash.statusGroup(g), dash.glyphCount(g))) {
              dash.statusGroupCount()++;
            } else {
              if (SkipItem(&arr) != CborNoError) return false;
            }
          }
          if (!SkipRest(&arr)) return false;
          if (cbor_value_leave_container(&m, &arr) != CborNoError) return false;
        }
      } else if (!cbor_value_at_end(&m)) {
        if (SkipItem(&m) != CborNoError) return false;
      }
      continue;
    }

    if (KeyIsPair(&m, HA_WIRE_KEY(DASHBOARD, "sensorGroups"))) {
      if (!SkipKey(&m)) return false;
      if (ArrayKeyTaken(&arraysSeen, kSeenSensorGroups)) {
        if (!SkipValueChecked(&m)) return false;
      } else if (!cbor_value_at_end(&m) && cbor_value_is_array(&m)) {
        CborValue arr;
        if (cbor_value_enter_container(&m, &arr) == CborNoError) {
          while (!cbor_value_at_end(&arr) && dash.sensorGroupCount() < kSensorGroupSlots) {
            const uint8_t g = dash.sensorGroupCount();
            if (DecodeSensorGroup(&arr, dash.sensorGroup(g), dash.valueCount(g))) {
              dash.sensorGroupCount()++;
            } else {
              if (SkipItem(&arr) != CborNoError) return false;
            }
          }
          if (!SkipRest(&arr)) return false;
          if (cbor_value_leave_container(&m, &arr) != CborNoError) return false;
        }
      } else if (!cbor_value_at_end(&m)) {
        if (SkipItem(&m) != CborNoError) return false;
      }
      continue;
    }

    if (KeyIsPair(&m, HA_WIRE_KEY(DASHBOARD, "weather"))) {
      if (!SkipKey(&m)) return false;
      if (!cbor_value_at_end(&m)) {
        if (cbor_value_is_null(&m)) {
          if (SkipItem(&m) != CborNoError) return false;
        } else if (cbor_value_is_map(&m)) {
          CborValue wm;
          if (cbor_value_enter_container(&m, &wm) == CborNoError) {
            while (!cbor_value_at_end(&wm)) {
              if (!cbor_value_is_text_string(&wm)) {
                if (!SkipUnknownEntry(&wm)) return false;
                continue;
              }
              if (KeyIsPair(&wm, HA_WIRE_KEY(WEATHER, "condition"))) { if (!SkipKey(&wm)) return false; DecodeStringField(&wm, HA_FIELD(dash.d, weatherCondition)); continue; }
              if (KeyIsPair(&wm, HA_WIRE_KEY(WEATHER, "hi"))) {
                if (!SkipKey(&wm)) return false;
                if (!cbor_value_at_end(&wm) && cbor_value_is_integer(&wm)) {
                  int64_t v = 0; cbor_value_get_int64(&wm, &v);
                  // REQ-027 — clamp to int8 range.
                  if (v < -128) v = -128;
                  if (v > 127) v = 127;
                  if (dash.d) dash.d->weatherHi = static_cast<int8_t>(v);
                }
                // Checked: a failed advance leaves the iterator inconsistent, and the
                // next at_end() then reports false — see SkipUnknownEntry.
                if (!SkipValueChecked(&wm)) return false;
                continue;
              }
              if (KeyIsPair(&wm, HA_WIRE_KEY(WEATHER, "lo"))) {
                if (!SkipKey(&wm)) return false;
                if (!cbor_value_at_end(&wm) && cbor_value_is_integer(&wm)) {
                  int64_t v = 0; cbor_value_get_int64(&wm, &v);
                  // REQ-027 — clamp to int8 range.
                  if (v < -128) v = -128;
                  if (v > 127) v = 127;
                  if (dash.d) dash.d->weatherLo = static_cast<int8_t>(v);
                }
                // Checked: a failed advance leaves the iterator inconsistent, and the
                // next at_end() then reports false — see SkipUnknownEntry.
                if (!SkipValueChecked(&wm)) return false;
                continue;
              }
              if (!SkipUnknownEntry(&wm)) return false;
            }
            if (cbor_value_leave_container(&m, &wm) != CborNoError) return false;
          }
        } else {
          if (SkipItem(&m) != CborNoError) return false;
        }
      }
      continue;
    }

    if (KeyIsPair(&m, HA_WIRE_KEY(DASHBOARD, "events"))) {
      if (!SkipKey(&m)) return false;
      if (ArrayKeyTaken(&arraysSeen, kSeenEvents)) {
        if (!SkipValueChecked(&m)) return false;
      } else if (!cbor_value_at_end(&m) && cbor_value_is_array(&m)) {
        CborValue arr;
        if (cbor_value_enter_container(&m, &arr) == CborNoError) {
          while (!cbor_value_at_end(&arr) && dash.eventCount() < kEventSlots) {
            if (DecodeEvent(&arr, dash.event(dash.eventCount()))) {
              dash.eventCount()++;
            } else {
              if (SkipItem(&arr) != CborNoError) return false;
            }
          }
          if (!SkipRest(&arr)) return false;
          if (cbor_value_leave_container(&m, &arr) != CborNoError) return false;
        }
      } else if (!cbor_value_at_end(&m)) {
        if (SkipItem(&m) != CborNoError) return false;
      }
      continue;
    }

    // Last-enriched-at epoch seconds (set by Android enrichment pipeline)
    if (KeyIsPair(&m, HA_WIRE_KEY(DASHBOARD, "lea"))) {
      if (!SkipKey(&m)) return false;
      if (!cbor_value_at_end(&m) && cbor_value_is_integer(&m)) {
        int64_t v = 0;
        cbor_value_get_int64(&m, &v);
        if (dash.d) dash.d->lastEnrichedAt = static_cast<uint32_t>(v);
      }
      // Checked: a failed advance leaves the iterator inconsistent, and the
      // next at_end() then reports false — see SkipUnknownEntry.
      if (!SkipValueChecked(&m)) return false;
      continue;
    }

    // v5: Configurable headline value
    if (KeyIsPair(&m, HA_WIRE_KEY(DASHBOARD, "cvl"))) {
      if (!SkipKey(&m)) return false;
      DecodeStringField(&m, dash.d ? &dash.d->configurableValue.label : nullptr);
      if (dash.d) dash.d->configurableValue.hasValue = true;
      continue;
    }
    if (KeyIsPair(&m, HA_WIRE_KEY(DASHBOARD, "cvv"))) {
      if (!SkipKey(&m)) return false;
      DecodeStringField(&m, dash.d ? &dash.d->configurableValue.value : nullptr);
      if (dash.d) dash.d->configurableValue.hasValue = true;
      continue;
    }

    // v5: Inline metric slot 0
    if (KeyIsPair(&m, HA_WIRE_KEY(DASHBOARD, "im0l"))) {
      if (!SkipKey(&m)) return false;
      DecodeStringField(&m, dash.d ? &dash.d->inlineMetrics[0].label : nullptr);
      if (dash.d) dash.d->inlineMetrics[0].enabled = true;
      continue;
    }
    if (KeyIsPair(&m, HA_WIRE_KEY(DASHBOARD, "im0v"))) {
      if (!SkipKey(&m)) return false;
      DecodeStringField(&m, dash.d ? &dash.d->inlineMetrics[0].value : nullptr);
      if (dash.d) dash.d->inlineMetrics[0].enabled = true;
      continue;
    }

    // v5: Inline metric slot 1
    if (KeyIsPair(&m, HA_WIRE_KEY(DASHBOARD, "im1l"))) {
      if (!SkipKey(&m)) return false;
      DecodeStringField(&m, dash.d ? &dash.d->inlineMetrics[1].label : nullptr);
      if (dash.d) dash.d->inlineMetrics[1].enabled = true;
      continue;
    }
    if (KeyIsPair(&m, HA_WIRE_KEY(DASHBOARD, "im1v"))) {
      if (!SkipKey(&m)) return false;
      DecodeStringField(&m, dash.d ? &dash.d->inlineMetrics[1].value : nullptr);
      if (dash.d) dash.d->inlineMetrics[1].enabled = true;
      continue;
    }

    if (!SkipUnknownEntry(&m)) return false;
  }

  return cbor_value_leave_container(iter, &m) == CborNoError;
}

}  // namespace

bool DecodeTileTo(CborValue* iter, LayoutService::WatchTile* tile) {
  return DecodeTile(iter, tile);
}

bool DecodeScreenTo(CborValue* iter, ScreenRef screen, DashRef dash) {
  if (!cbor_value_is_map(iter)) return false;
  CborValue m;
  if (cbor_value_enter_container(iter, &m) != CborNoError) return false;
  uint8_t itemsSeen = 0;  // array-key claim (duplicate rule, LayoutDecoder.h)

  while (!cbor_value_at_end(&m)) {
    if (!cbor_value_is_text_string(&m)) {
      if (!SkipUnknownEntry(&m)) return false;
      continue;
    }
    bool match = false;

    if (KeyIsPair(&m, HA_WIRE_KEY(SCREEN, "screenType"))) {
      if (!SkipKey(&m)) return false;
      if (!cbor_value_at_end(&m) && cbor_value_is_integer(&m)) {
        int64_t v = 0;
        cbor_value_get_int64(&m, &v);
        // Range-checked before narrowing: 256 must not wrap to 0 (a dashboard).
        // Any value outside 0..255 saturates to 255, which like every non-zero
        // type is not a dashboard.
        screen.screenType() = (v < 0 || v > 255) ? static_cast<uint8_t>(255) : static_cast<uint8_t>(v);
      }
      // Checked: a failed advance leaves the iterator inconsistent, and the
      // next at_end() then reports false — see SkipUnknownEntry.
      if (!SkipValueChecked(&m)) return false;
      continue;
    }

    if (KeyIsPair(&m, HA_WIRE_KEY(SCREEN, "tileLayout"))) {
      if (!SkipKey(&m)) return false;
      if (!cbor_value_at_end(&m) && cbor_value_is_integer(&m)) {
        int64_t v = 4;
        cbor_value_get_int64(&m, &v);
        if (v == 4 && screen.w) screen.w->tileLayout = static_cast<uint8_t>(v);  // only 2x2 (4) supported in v2
      }
      // Checked: a failed advance leaves the iterator inconsistent, and the
      // next at_end() then reports false — see SkipUnknownEntry.
      if (!SkipValueChecked(&m)) return false;
      continue;
    }

    if (KeyIsPair(&m, HA_WIRE_KEY(SCREEN, "tiles"))) {
      if (!SkipKey(&m)) return false;
      if (ArrayKeyTaken(&itemsSeen, kSeenItems)) {
        if (!SkipValueChecked(&m)) return false;
      } else if (!cbor_value_at_end(&m) && cbor_value_is_array(&m)) {
        CborValue arr;
        if (cbor_value_enter_container(&m, &arr) == CborNoError) {
          while (!cbor_value_at_end(&arr) && screen.tileCount() < LayoutService::kMaxTilesPerScreen) {
            if (DecodeTile(&arr, screen.tile(screen.tileCount()))) {
              screen.tileCount()++;
            } else {
              if (SkipItem(&arr) != CborNoError) return false;
            }
          }
          if (!SkipRest(&arr)) return false;
          if (cbor_value_leave_container(&m, &arr) != CborNoError) return false;
        }
      } else if (!cbor_value_at_end(&m)) {
        if (SkipItem(&m) != CborNoError) return false;
      }
      continue;
    }

    // "actions" removed in v2 — skip the CBOR value for forward compat
    cbor_value_text_string_equals(&m, "actions", &match);
    if (match) {
      if (!SkipUnknownEntry(&m)) return false;
      continue;
    }

    if (KeyIsPair(&m, HA_WIRE_KEY(SCREEN, "dashboard"))) {
      if (!SkipKey(&m)) return false;
      if (!cbor_value_at_end(&m)) {
        if (cbor_value_is_map(&m) && dash.present()) {
          if (!DecodeDashboard(&m, dash)) return false;
        } else {
          if (SkipItem(&m) != CborNoError) return false;
        }
      }
      continue;
    }

    if (!SkipUnknownEntry(&m)) return false;
  }

  return cbor_value_leave_container(iter, &m) == CborNoError;
}

#undef HA_FIELD

// Public wrappers: the original object-per-call API, always write mode.
bool DecodeTileInline(CborValue* iter, LayoutService::WatchTile& tile) {
  return DecodeTile(iter, &tile);
}

bool DecodeScreenInline(CborValue* iter, LayoutService::WatchScreen& screen, LayoutService::Dashboard* dashOut) {
  return DecodeScreenTo(iter, ScreenRef{&screen, nullptr}, DashRef{dashOut, nullptr});
}

}
}
}
