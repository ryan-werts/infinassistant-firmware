#include "ha_bridge/LayoutDocumentDecoder.h"

#include <array>
#include <climits>
#include <cstring>
#include <tuple>

#include "DashboardLimits.h"

// The layout-level key loop and the post-loop tail of the old
// HandleWatchFaceLayout, moved here unchanged in behaviour (REQ009-FR-C8a).
// See the header for the two modes and the sink contract.
//
// Every skip on this path is SkipItem / SkipUnknownEntry (LayoutDecoder.cpp):
// iterative and depth-capped, never tinycbor's recursive cbor_value_advance.
//
// Every key is matched in its long and short spelling (KeyIsPair + HA_WIRE_KEY,
// LayoutDecoder.h, where the duplicate rule is stated): a repeated scalar is
// last-wins, and a repeated array key (screens) keeps the first occurrence and
// skips the later ones, whichever spelling each uses. The claim is per containing
// map: DecodeLayoutMap holds the layout-level bits.
//
// Keys the app no longer sends (theme, statusBar, the header's timeAlign and
// presenceIcons, and the v1 layout-level tiles) have no decoder: they are unknown
// keys, skipped whole like any other.

namespace Pinetime {
namespace Applications {
namespace HaBridge {
namespace {

using Layout = LayoutService::WatchFaceLayout;

constexpr uint8_t kMaxScreens = LayoutService::kMaxScreens;

// Capacity of each LayoutText slot, in LayoutText order. DecodeStringInto
// rejects a string that does not fit, so this is part of the wire semantics.
constexpr size_t kTextCap[] = {
  std::tuple_size<decltype(Layout::timeFormat)>::value,
  std::tuple_size<decltype(Layout::defaultActiveColor)>::value,
  std::tuple_size<decltype(Layout::defaultInactiveColor)>::value,
};
constexpr size_t kTextCount = sizeof(kTextCap) / sizeof(kTextCap[0]);

// Bits of Doc::zeroed: bits 0..kMaxScreens-1 are the screen slots, the top bit
// is the dashboard slot.
constexpr uint8_t kDashBit = 7;
static_assert(kMaxScreens <= kDashBit, "the dashboard bit must not collide with a screen slot bit");

constexpr uint8_t kNone = 0xFF;

// Each sub-map / array decoder keeps its own small frame instead of being
// merged into the entry function: the decode runs on the NimBLE host task stack,
// where only the deepest chain counts, and a merged frame is the sum of every
// branch's locals.
#define HA_NOINLINE __attribute__((noinline))

template <typename T>
void ZeroInPlace(T* p) {
  std::memset(static_cast<void*>(p), 0, sizeof(T));
}

// Everything the walk remembers for a target it has no slot for: the counters
// and screen types it steers by (LayoutDecoder.h). About 20 B; it replaces the
// 1100 B scratch copy of the layout the first Validate implementation needed.
// Validate mode uses it for every target; Write mode only for a target the sink
// declined (a getter that returned nullptr).
struct Shadow {
  ScreenShadow screen[kMaxScreens];
  DashShadow dash;
};

// Decode state, and the one place a target is chosen. In Write mode a slot comes
// from the sink and is zeroed in place the first time this decode touches it; a
// slot the sink declines (nullptr) is walked through the shadow counters instead,
// exactly as in Validate mode. In Validate mode (`validate`) no slot is ever
// asked for: the Ref handed out carries only the shadow counters, which start at
// zero and so need no reset.
struct Doc {
  LayoutSink& sink;
  Shadow* shadow;           // always set; zeroed by the entry point
  bool validate;
  int version;
  uint8_t screenCount;
  uint8_t dashboardScreenIndex;
  bool hasDashboard;
  bool rangeBad;            // a REQ-020 range check failed on some item
  bool screensTrimmed;      // the screens array held more than kMaxScreens
  uint8_t zeroed;           // Write mode: slots already zeroed
  uint8_t tileMask;         // bit k: screen k decoded with screenType != 0

  Doc(LayoutSink& s, Shadow* sh, bool v)
    : sink(s), shadow(sh), validate(v), version(0), screenCount(0), dashboardScreenIndex(0),
      hasDashboard(false), rangeBad(false), screensTrimmed(false), zeroed(0), tileMask(0) {}

  // Layout-level text: nullptr in Validate mode, and for a target the sink
  // declines (DecodeStringInto takes a null target as no-write).
  char* Text(LayoutText which) { return validate ? nullptr : sink.Text(which); }

  ScreenRef Screen(uint8_t index) {
    if (validate) return ScreenRef{nullptr, &shadow->screen[index]};
    LayoutService::WatchScreen* slot = sink.ScreenSlot(index);
    if (slot == nullptr) return ScreenRef{nullptr, &shadow->screen[index], &sink, index};
    if ((zeroed & (1u << index)) == 0) {
      ZeroInPlace(slot);
      zeroed = static_cast<uint8_t>(zeroed | (1u << index));
    }
    return ScreenRef{slot, nullptr};
  }

  DashRef Dash() {
    if (validate) return DashRef{nullptr, &shadow->dash};
    LayoutService::Dashboard* slot = sink.DashboardSlot();
    if (slot == nullptr) return DashRef{nullptr, &shadow->dash};
    if ((zeroed & (1u << kDashBit)) == 0) {
      ZeroInPlace(slot);
      zeroed = static_cast<uint8_t>(zeroed | (1u << kDashBit));
    }
    return DashRef{slot, nullptr};
  }
};

// REQ-020 — semantic range gates, sourced from DashboardLimits (single source
// of truth) and from the layout struct's own array bounds. The decoders already
// cap every count while decoding; these are the backstop the handler always ran.
// They read the counts through the Ref, so they judge Write and Validate alike.
bool InRange(const ScreenRef& screen) {
  return screen.tileCount() <= std::tuple_size<decltype(LayoutService::WatchScreen::tiles)>::value;
}

bool InRange(const DashRef& dash) {
  if (dash.statusGroupCount() > DashboardLimits::MAX_STATUS_GROUPS) return false;
  if (dash.sensorGroupCount() > DashboardLimits::MAX_SENSOR_GROUPS) return false;
  for (uint8_t g = 0; g < dash.statusGroupCount(); ++g) {
    if (*dash.glyphCount(g) > DashboardLimits::MAX_GLYPHS_PER_GROUP) return false;
  }
  for (uint8_t g = 0; g < dash.sensorGroupCount(); ++g) {
    if (*dash.valueCount(g) > DashboardLimits::MAX_SENSORS_PER_GROUP) return false;
  }
  return true;
}

// What a WalkMapValue entry callback reports for a text key.
enum class Entry : uint8_t {
  Unknown,  // not a key of this map: the entry is skipped
  Handled,  // the key and its value were consumed
  Failed,   // the iterator is no longer usable (a failed skip): stop the walk
};

// Walk the value at `it` if it is a map, calling `entry` for each text key. A
// value that is not a map is skipped whole. Returns false when the iterator is
// no longer usable (a failed skip).
template <typename F>
bool WalkMapValue(CborValue* it, F&& entry) {
  if (!cbor_value_at_end(it) && cbor_value_is_map(it)) {
    CborValue m;
    if (cbor_value_enter_container(it, &m) == CborNoError) {
      while (!cbor_value_at_end(&m)) {
        if (!cbor_value_is_text_string(&m)) {
          if (!SkipUnknownEntry(&m)) return false;
          continue;
        }
        const Entry e = entry(&m);
        if (e == Entry::Failed) return false;
        if (e == Entry::Handled) continue;
        if (!SkipUnknownEntry(&m)) return false;
      }
      if (cbor_value_leave_container(it, &m) != CborNoError) return false;
    }
  } else if (!cbor_value_at_end(it)) {
    if (SkipItem(it) != CborNoError) return false;
  }
  return true;
}

// The header map: timeFormat only. timeAlign and presenceIcons are skipped as unknown keys.
HA_NOINLINE bool DecodeHeader(Doc& d, CborValue* it) {
  return WalkMapValue(it, [&d](CborValue* m) {
    if (KeyIsPair(m, HA_WIRE_KEY(HEADER, "timeFormat"))) {
      if (!SkipKey(m)) return Entry::Failed;
      DecodeStringInto(m, d.Text(LayoutText::TimeFormat), kTextCap[static_cast<size_t>(LayoutText::TimeFormat)]);
      return Entry::Handled;
    }
    return Entry::Unknown;
  });
}

// "screens" (v2 multi-screen format)
HA_NOINLINE bool DecodeScreens(Doc& d, CborValue* it) {
  if (!cbor_value_at_end(it) && cbor_value_is_array(it)) {
    CborValue arr;
    if (cbor_value_enter_container(it, &arr) == CborNoError) {
      while (!cbor_value_at_end(&arr) && d.screenCount < kMaxScreens) {
        const ScreenRef screen = d.Screen(d.screenCount);
        if (DecodeScreenTo(&arr, screen, d.Dash())) {
          if (screen.screenType() == 0) {
            d.hasDashboard = true;
            d.dashboardScreenIndex = d.screenCount;
          } else {
            d.tileMask = static_cast<uint8_t>(d.tileMask | (1u << d.screenCount));
          }
          if (!InRange(screen) || (d.hasDashboard && !InRange(d.Dash()))) d.rangeBad = true;
          d.screenCount++;
        } else {
          if (SkipItem(&arr) != CborNoError) return false;
        }
      }
      if (!cbor_value_at_end(&arr)) d.screensTrimmed = true;
      if (!SkipRest(&arr)) return false;
      if (cbor_value_leave_container(it, &arr) != CborNoError) return false;
    }
  } else if (!cbor_value_at_end(it)) {
    if (SkipItem(it) != CborNoError) return false;
  }
  return true;
}

// The layout-level key loop. Returns false when the iterator is unusable.
bool DecodeLayoutMap(Doc& d, CborValue* it) {
  uint8_t seen = 0;  // layout-level array-key claims (duplicate rule, LayoutDecoder.h)
  while (!cbor_value_at_end(it)) {
    if (!cbor_value_is_text_string(it)) {
      if (!SkipUnknownEntry(it)) return false;
      continue;
    }

    if (KeyIsPair(it, HA_WIRE_KEY(LAYOUT, "version"))) {
      if (!SkipKey(it)) return false;
      if (!cbor_value_at_end(it) && cbor_value_is_integer(it)) {
        int64_t v = 0;
        cbor_value_get_int64(it, &v);
        // Range-checked before narrowing: a 64-bit value that only wraps into
        // 1..255 (0x1_0000_0001) must not pass as a valid version. Saturated to
        // the int range, it fails the 1..255 check after the loop like any
        // other out-of-range version.
        if (v > INT_MAX) {
          d.version = INT_MAX;
        } else if (v < INT_MIN) {
          d.version = INT_MIN;
        } else {
          d.version = static_cast<int>(v);
        }
      }
      if (!cbor_value_at_end(it) && SkipItem(it) != CborNoError) return false;
      continue;
    }

    if (KeyIsPair(it, HA_WIRE_KEY(LAYOUT, "header"))) {
      if (!SkipKey(it)) return false;
      if (!DecodeHeader(d, it)) return false;
      continue;
    }

    if (KeyIsPair(it, HA_WIRE_KEY(LAYOUT, "screens"))) {
      if (!SkipKey(it)) return false;
      if (ArrayKeyTaken(&seen, kSeenScreens)) {
        if (!cbor_value_at_end(it) && SkipItem(it) != CborNoError) return false;
      } else {
        if (!DecodeScreens(d, it)) return false;
      }
      continue;
    }

    if (KeyIsPair(it, HA_WIRE_KEY(LAYOUT, "defaultActiveColor"))) {
      if (!SkipKey(it)) return false;
      if (!cbor_value_at_end(it)) {
        DecodeStringInto(it, d.Text(LayoutText::DefaultActiveColor),
                         kTextCap[static_cast<size_t>(LayoutText::DefaultActiveColor)]);
      }
      continue;
    }

    if (KeyIsPair(it, HA_WIRE_KEY(LAYOUT, "defaultInactiveColor"))) {
      if (!SkipKey(it)) return false;
      if (!cbor_value_at_end(it)) {
        DecodeStringInto(it, d.Text(LayoutText::DefaultInactiveColor),
                         kTextCap[static_cast<size_t>(LayoutText::DefaultInactiveColor)]);
      }
      continue;
    }

    // "actions" was removed in v2; skip the value for forward compatibility.
    // An unknown key (theme, statusBar and the v1 tiles among them) is skipped
    // the same way.
    if (!SkipUnknownEntry(it)) return false;
  }
  return true;
}

// Write mode: absent layout-level fields decode to their zero value, so zero
// the text slots in place before the loop.
void ResetLayoutFields(Doc& d) {
  for (size_t t = 0; t < kTextCount; ++t) {
    char* text = d.Text(static_cast<LayoutText>(t));
    if (text != nullptr) std::memset(text, 0, kTextCap[t]);
  }
}

// Absent default colours take their fixed defaults (Write mode only).
void FillDefaultColors(Doc& d) {
  static constexpr struct {
    LayoutText which;
    const char* value;
  } kDefaults[] = {
    {LayoutText::DefaultActiveColor, "#FFD700"},
    {LayoutText::DefaultInactiveColor, "#800080"},
  };
  for (const auto& def : kDefaults) {
    char* text = d.Text(def.which);
    if (text == nullptr || text[0] != '\0') continue;
    const size_t cap = kTextCap[static_cast<size_t>(def.which)];
    std::strncpy(text, def.value, cap - 1);
    text[cap - 1] = '\0';
  }
}

}  // namespace

LayoutDecodeResult DecodeLayoutDocument(const uint8_t* payload, size_t len, LayoutSink& sink, LayoutDecodeMode mode) {
  CborParser parser;
  CborValue root, mapIter;

  if (cbor_parser_init(payload, len, 0, &parser, &root) != CborNoError || !cbor_value_is_map(&root)) {
    return {LayoutVerdict::DecodeError, 0};
  }

  // Structural pre-pass: skip the whole root item once, on a copy, before the
  // sink is touched. A truncated or otherwise malformed document fails here, so
  // it can never reach a sink call (no partial commit), and one that passes is
  // complete to tinycbor's advance level (every container closed, every string
  // whole), so the walk below cannot meet an EOF mid-container. The same skip
  // enforces the depth cap (DashboardLimits::MAX_CBOR_DEPTH). Iterative, linear
  // in the payload, no allocation; `mapIter` is only the scratch copy here, it
  // is overwritten by the enter below.
  mapIter = root;
  if (SkipItem(&mapIter) != CborNoError) return {LayoutVerdict::DecodeError, 0};

  // The counters of every target nobody lent a slot for, zeroed: all of them in
  // Validate mode, the declined ones in Write mode.
  Shadow shadow;
  std::memset(&shadow, 0, sizeof(shadow));
  const bool validate = mode == LayoutDecodeMode::Validate;
  if (!validate && !sink.Open()) return {LayoutVerdict::NoTarget, 0};

  Doc d(sink, &shadow, validate);
  if (!validate) ResetLayoutFields(d);

  if (cbor_value_enter_container(&root, &mapIter) != CborNoError) return {LayoutVerdict::DecodeError, d.version};
  if (!DecodeLayoutMap(d, &mapIter)) return {LayoutVerdict::DecodeError, d.version};

  if (!validate) FillDefaultColors(d);

  // REQ-020 — semantic validation gate: reject the push on any out-of-range
  // value, so a malformed payload cannot turn into bizarre layout state.
  if (d.version < 1 || d.version > 255) return {LayoutVerdict::ValidationError, d.version};
  if (d.screenCount > kMaxScreens || d.rangeBad) return {LayoutVerdict::ValidationError, d.version};

  // A structurally valid payload with no "version" key is malformed or comes
  // from a broken sender. NOTE: version 0 is already caught by the range check
  // above (0 < 1), so a missing version reports validation_error and this check
  // cannot fire; it is carried over as it stood.
  if (d.version == 0) return {LayoutVerdict::DecodeError, 0};

  if (!validate) {
    for (uint8_t i = 0; i < d.screenCount; ++i) {
      sink.SetScreenShape(i, shadow.screen[i].screenType, shadow.screen[i].tileCount);
    }
    sink.SetScreenTypes(d.tileMask);
    sink.SetCounts(d.screenCount, d.hasDashboard, d.dashboardScreenIndex);
    // Version LAST: torn-read guard (see LayoutSink::SetVersion).
    sink.SetVersion(static_cast<uint8_t>(d.version));
  }
  return {LayoutVerdict::Ok, d.version, d.screensTrimmed};
}

}
}
}
