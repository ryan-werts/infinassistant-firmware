#pragma once

// Watch-face layout CBOR decoding, split out of BridgeController.cpp.
//
// Two reasons this is its own translation unit:
//
//  1. It is the LIVE decoder. tools/fuzz targets
//     WatchFaceLayoutMessage::DecodeCbor, which BridgeController.cpp itself
//     documents as replaced by this in-place decoder — so the fuzzer has been
//     exercising dead code. None of this could be fuzzed while it sat static
//     inside a 4000-line file that includes FreeRTOS, LVGL and nrf.h.
//
//  2. Nothing in it needs any of those. These functions touch tinycbor and the
//     layout structs and nothing else, so on their own they build for the host
//     with only small NimBLE/FreeRTOS stubs for HomeAssistantService.h.
//
// Decoding is deliberately allocation-free: strings go straight into the
// fixed-size arrays of the layout struct via DecodeStringInto, and every
// collection loop is bounded by its backing std::array size, so an oversized
// payload truncates rather than overflowing.
//
// Declared in namespace Pinetime::Applications::HaBridge, the same namespace
// BridgeController.cpp's body sits in, so its call sites need no qualification
// and no using-declarations (a `using Pinetime::...` inside that namespace
// resolves relative to it and breaks every later lookup).

#include <cbor.h>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include "../components/ble/HomeAssistantService.h"
#include "DashboardLimits.h"

namespace Pinetime {
namespace Applications {
namespace HaBridge {

using LayoutService = Pinetime::Controllers::HomeAssistantService;

// ---------------------------------------------------------------------------
// Dual-key decode (REQ009-FR-C2)
//
// Every key a decoder matches is accepted in its long AND its short spelling,
// both read from the one generated table (WireShortKeys, DashboardLimits.h);
// no short key is written out by hand in a decoder. A key with no entry in the
// level's table does not compile (HA_WIRE_KEY), so a decoder cannot match a key
// the registry does not know. Long-only keys (logbook and friends) have no
// short form and reach the unknown-key skip like any other key.
//
// Duplicates. The two spellings of a key are one key, so all of the following
// hold whichever spelling each copy uses, at every level:
//
//   Scalar key    Every occurrence runs the same handler over the same field,
//                 so the LAST occurrence wins: a later text value replaces the
//                 earlier one (a later value that is too long, or not text,
//                 leaves the field empty, exactly as a lone bad value does), a
//                 later number or bool replaces it when it has the right type.
//                 A repeated map key (header, weather, dashboard) decodes again over the same target, field by
//                 field, under the same rule. Nothing accumulates.
//   Array key     The FIRST occurrence is decoded; every later occurrence of
//                 that key, in either spelling, is skipped whole with SkipItem.
//                 The first occurrence claims the key whatever its value type.
//                 That covers screens, a screen's tiles, statusGroups,
//                 sensorGroups, events, glyphs and sensors. "Later occurrence" is
//                 within one map: the claim is a local byte of the decoder that
//                 walks that map. Two different maps that each carry the array
//                 (a repeated header or dashboard map, two dashboard screens)
//                 each decode theirs, appending under the array's own
//                 capacity check, exactly as before.
//
// Why first-wins for arrays: keeping the first copy never rewinds a count, so
// nothing already stored is rewritten, no slot is decoded twice, and nothing
// needs resetting (a reset would have to re-zero screen slots and undo
// hasDashboard / dashboardScreenIndex). The claim is a semantic rule only:
// the bound on every array is still its own `count < capacity` loop guard,
// which no key spelling can bypass. Write and Validate share the one parser,
// so they apply the rule identically.
// ---------------------------------------------------------------------------

// Match the text key at `it` against either spelling of `k`.
inline bool KeyIsPair(CborValue* it, const ShortKeyPair& k) {
  bool match = false;
  cbor_value_text_string_equals(it, k.longKey, &match);
  if (!match) cbor_value_text_string_equals(it, k.shortKey, &match);
  return match;
}

constexpr bool WireStrEq(const char* a, const char* b) {
  while (*a != '\0' && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

// Position of `longKey` in `table`, or N when absent.
template <std::size_t N>
constexpr std::size_t WireKeyIndex(const ShortKeyPair (&table)[N], const char* longKey) {
  for (std::size_t i = 0; i < N; ++i) {
    if (WireStrEq(table[i].longKey, longKey)) return i;
  }
  return N;
}

template <std::size_t I, std::size_t N>
constexpr const ShortKeyPair& WirePair(const ShortKeyPair (&table)[N]) {
  static_assert(I < N, "key is not in the WireShortKeys table for this level");
  return table[I];
}

// The table entry for a long key, resolved at compile time. LEVEL is a
// WireShortKeys member name (TILE, DASHBOARD, ...).
#define HA_WIRE_KEY(LEVEL, longKey) \
  (::Pinetime::Applications::HaBridge::WirePair< \
      ::Pinetime::Applications::HaBridge::WireKeyIndex(::Pinetime::Applications::HaBridge::WireShortKeys::LEVEL, longKey)>( \
      ::Pinetime::Applications::HaBridge::WireShortKeys::LEVEL))

// Array-key claim bits (see the duplicate rule above): one local byte per
// containing map, in the decoder that walks that map.
constexpr uint8_t kSeenScreens = 1u << 0;
constexpr uint8_t kSeenStatusGroups = 1u << 0;
constexpr uint8_t kSeenSensorGroups = 1u << 1;
constexpr uint8_t kSeenEvents = 1u << 2;
constexpr uint8_t kSeenItems = 1u << 0;  // a screen's tiles, a group's glyphs or sensors

// True when the key was already claimed (the caller skips this copy); otherwise
// claims it and returns false.
inline bool ArrayKeyTaken(uint8_t* seen, uint8_t bit) {
  if ((*seen & bit) != 0) return true;
  *seen = static_cast<uint8_t>(*seen | bit);
  return false;
}

/**
 * Skip exactly one CBOR data item (a scalar, a string of any chunking, or a
 * whole map / array), advancing `it` to the item after it. The same contract
 * as cbor_value_advance(), but iterative and depth-capped: tinycbor's version
 * recurses once per nesting level (160 B of stack each, limit 1024), which on
 * the NimBLE host task stack is what made the GATT-callback chain exceed its
 * budget. This one holds one CborValue per open container in a fixed array
 * (DashboardLimits::MAX_CBOR_DEPTH entries), never recurses, and returns
 * CborErrorNestingTooDeep beyond that depth (the handler's pre-scan already
 * rejects payloads nested deeper than the same limit).
 *
 * Every skip on the layout decode path goes through this; none calls
 * cbor_value_advance().
 *
 * Total on malformed input: an iterator that is at the end, or is no longer
 * valid (a container cut off before its next promised item leaves it
 * CborInvalidType), returns an error instead of reaching tinycbor's advance,
 * which asserts on such an iterator (undefined behaviour with -DNDEBUG). Every
 * call either advances past one item and returns CborNoError, or returns an
 * error, so a loop that stops on a failed skip cannot spin.
 */
CborError SkipItem(CborValue* it);

/** SkipItem for the key a walk has just matched: false when the iterator is unusable. */
inline bool SkipKey(CborValue* it) { return SkipItem(it) == CborNoError; }

/**
 * Skip everything left in the container `it` walks. False when a skip fails
 * (a truncated container); the caller then returns without leaving it.
 */
inline bool SkipRest(CborValue* it) {
  while (!cbor_value_at_end(it)) {
    if (SkipItem(it) != CborNoError) return false;
  }
  return true;
}

/**
 * Skip a map entry whose key is not recognised, advancing past key and value.
 * Returns false if the iterator is not usable afterwards — a failed advance
 * leaves it inconsistent, and cbor_value_at_end() then reports false, so an
 * unchecked second advance walks off the end of the buffer. ASAN caught
 * exactly that via fuzz_layout_screen.
 *
 * Shared with BridgeController.cpp, whose CONFIG_PUSH / NOTIFY / LOGBOOK /
 * CALENDAR / HOME_CONTROL decoders used the same unchecked idiom.
 */
bool SkipUnknownEntry(CborValue* it);

/**
 * Decode a CBOR text string into a fixed-size char array.
 *
 * Rejects anything that would not fit (strLen >= outSize) rather than
 * truncating, always NUL-terminates, and ALWAYS advances the iterator so a
 * malformed payload cannot spin a decode loop forever.
 *
 * `out` may be nullptr (no-write mode): the string is measured and checked
 * against `outSize` and the iterator moves exactly as it would on a write, but
 * nothing is stored.
 */
bool DecodeStringInto(CborValue* value, char* out, size_t outSize);

/** Decode one tile object into `tile`. */
bool DecodeTileInline(CborValue* iter, LayoutService::WatchTile& tile);

/**
 * Decode one screen object into `screen`, writing any dashboard it carries
 * into `dashOut` (the dashboard is hoisted to layout level rather than stored
 * per screen, so screens[] stays small).
 */
bool DecodeScreenInline(CborValue* iter,
                        LayoutService::WatchScreen& screen,
                        LayoutService::Dashboard* dashOut);

// ---------------------------------------------------------------------------
// No-write mode
//
// Every per-item decoder above is a thin wrapper over one implementation that
// takes an OPTIONAL target: with a target it writes, without one it parses,
// range-checks and counts exactly the same way and stores nothing. The
// document decoder's Validate pass uses this, so a validate pass needs no
// scratch copy of the layout (REQ009-FR-C8a, REQ009-NFR-4) and cannot drift
// from the write pass: there is one parser.
//
// What a walk reads back from its own output is only the counts (how many
// tiles / groups / glyphs / sensors / events it has taken so
// far) and the screen type. In no-write mode those live in the Shadow structs
// below instead of in the layout structs. The Ref types pick one or the other:
// a write ref points at the real target, a shadow ref at the counters.
// ---------------------------------------------------------------------------

struct ScreenShadow {
  uint8_t tileCount;
  uint8_t screenType;
};

struct DashShadow {
  uint8_t eventCount;
  uint8_t statusGroupCount;
  uint8_t sensorGroupCount;
  uint8_t glyphCount[LayoutService::kMaxStatusGroups];
  uint8_t valueCount[3];
};
static_assert(std::tuple_size<decltype(LayoutService::Dashboard::sensorGroups)>::value == 3,
              "DashShadow::valueCount must have one entry per sensor group");

// Lends one tile of a screen that has no screen slot. Asked once per tile
// attempt, with the tile index the walk is about to fill; nullptr means the
// tile is parsed and range-checked but not stored. The same index is asked again
// after a failed attempt, and a later index only after the tile count moved on,
// so a lender may hold a single tile and hand it on when the index advances.
struct TileLender {
  virtual LayoutService::WatchTile* Lend(uint8_t screen, uint8_t tile) {
    (void) screen;
    (void) tile;
    return nullptr;
  }

 protected:
  ~TileLender() = default;
};

// A screen to decode into: `w` is the slot to fill (write mode) or nullptr and
// `s` the counters (no-write mode). Exactly one of the two is set. With no slot
// (`w` nullptr) the tiles come from `lender` when one is set (screen `index`),
// and are not stored otherwise.
struct ScreenRef {
  LayoutService::WatchScreen* w;
  ScreenShadow* s;
  TileLender* lender = nullptr;
  uint8_t index = 0;

  uint8_t& tileCount() const { return w ? w->tileCount : s->tileCount; }
  uint8_t& screenType() const { return w ? w->screenType : s->screenType; }
  LayoutService::WatchTile* tile(uint8_t i) const {
    if (w) return &w->tiles[i];
    return lender ? lender->Lend(index, i) : nullptr;
  }
};

// The dashboard to decode into. Write mode sets `d`, no-write mode sets `s`;
// both nullptr means "no dashboard target": a dashboard in the payload is then
// skipped whole.
struct DashRef {
  LayoutService::Dashboard* d;
  DashShadow* s;

  bool present() const { return d != nullptr || s != nullptr; }

  uint8_t& eventCount() const { return d ? d->eventCount : s->eventCount; }
  uint8_t& statusGroupCount() const { return d ? d->statusGroupCount : s->statusGroupCount; }
  uint8_t& sensorGroupCount() const { return d ? d->sensorGroupCount : s->sensorGroupCount; }

  LayoutService::CalendarEvent* event(uint8_t i) const { return d ? &d->events[i] : nullptr; }
  LayoutService::StatusGroup* statusGroup(uint8_t i) const { return d ? &d->statusGroups[i] : nullptr; }
  LayoutService::SensorGroup* sensorGroup(uint8_t i) const { return d ? &d->sensorGroups[i] : nullptr; }
  uint8_t* glyphCount(uint8_t i) const { return d ? &d->statusGroups[i].glyphCount : &s->glyphCount[i]; }
  uint8_t* valueCount(uint8_t i) const { return d ? &d->sensorGroups[i].valueCount : &s->valueCount[i]; }
};

/** DecodeScreenInline, with the screen and dashboard as write or no-write refs. */
bool DecodeScreenTo(CborValue* iter, ScreenRef screen, DashRef dash);

/** DecodeTileInline, `tile` optional (nullptr = no-write). */
bool DecodeTileTo(CborValue* iter, LayoutService::WatchTile* tile);

}
}
}
