#pragma once

// Layout-level WATCHFACE_LAYOUT decode, split out of BridgeController.cpp
// (REQ009-FR-C8a).
//
// LayoutDecoder.{h,cpp} decodes the OBJECTS of a layout (a screen, a tile, a
// dashboard). This unit decodes the DOCUMENT around them: the layout-level
// map, its `header` sub-map and the `screens` array, and everything the handler
// used to do after the key loop (default colours, the REQ-020 range checks and
// the version checks). HandleWatchFaceLayout keeps only frame dispatch, the
// WATCHFACE_ACK and the seqlock publish calls.
//
// One code path, two modes:
//
//   Write     decodes into the caller's LayoutSink, one item at a time.
//   Validate  runs the identical loop and the identical checks but never calls
//             a sink hook (Open, Text, the slot getters, SetCounts,
//             SetVersion) and needs no memory of its own beyond a few counters
//             on the stack (about 20 B, see Shadow in the .cpp): every
//             per-item decoder has a no-write mode on the same code path
//             (LayoutDecoder.h), which parses, range-checks and counts without
//             storing. The verdict is the one Write mode would reach, because
//             it is the same parser.
//
// No check depends on key order: CBOR map order is not fixed, so `version` may
// follow `screens`. The range checks run per item as it is decoded, the tail
// checks run once the whole map has been walked, and nothing is published
// (SetCounts, SetVersion) until every check has passed.
//
// The unit is allocation-free. It resets each decode target in place, item by
// item, on first use; it never resets by assigning a temporary (REQ009-NFR-4).
// On firmware the sink is a per-item writer, never a whole WatchFaceLayout.
// WatchFaceLayoutSink below adapts a resident WatchFaceLayout, which is the
// host full-struct target and, until 7b.1, the transitional firmware target.
//
// Declared in the same namespace as LayoutDecoder.h; include it at file scope
// (see the note there).

#include <cbor.h>
#include <cstddef>
#include <cstdint>
#include "LayoutDecoder.h"

namespace Pinetime {
namespace Applications {
namespace HaBridge {

enum class LayoutDecodeMode : uint8_t { Write, Validate };

enum class LayoutVerdict : uint8_t {
  Ok,
  DecodeError,       // ack reason "decode_error"
  ValidationError,   // ack reason "validation_error"
  NoTarget           // ack reason "oom": Write mode, the sink could not be opened
};

struct LayoutDecodeResult {
  LayoutVerdict verdict;
  // The version decoded up to the point the verdict was reached (0 when none):
  // what the WATCHFACE_ACK reports on a rejection.
  int version;
  // The screens array held more screens than the format takes (kMaxScreens): the
  // surplus was skipped. The verdict is still Ok; the ACK reports it.
  bool screensTrimmed = false;
};

// Layout-level text fields. Capacity of each is fixed by the layout format
// (16, 8, 8); a sink slot must hold at least that many bytes.
enum class LayoutText : uint8_t {
  TimeFormat,
  DefaultActiveColor,
  DefaultInactiveColor
};

// The per-item target of a Write-mode decode. Slot getters return a writable
// slot that stays valid until the decode returns; the unit zeroes it in place
// before first use, so a sink need not hand out pre-zeroed storage. Indexes
// are always below the format's caps (kMaxScreens).
//
// A sink may DECLINE any target by returning nullptr from its getter (Text,
// ScreenSlot, DashboardSlot). The unit then walks that
// item exactly as Validate mode walks it, keeping only the counters the walk
// steers by, so the verdict, the version and every OTHER target are the same
// as with a full sink. A pass that needs one section lends only that section's
// target. Declining a target does not change what the unit writes to the
// targets a sink does lend.
class LayoutSink : public TileLender {
 public:
  // Called once, after the payload is known to be a CBOR map and before the
  // first write. Returning false ends the decode with LayoutVerdict::NoTarget.
  virtual bool Open() = 0;
  virtual char* Text(LayoutText which) = 0;
  virtual LayoutService::WatchScreen* ScreenSlot(uint8_t index) = 0;
  // The single dashboard (only one dashboard screen is supported).
  virtual LayoutService::Dashboard* DashboardSlot() = 0;
  // A sink that declines a screen slot may lend that screen's tiles one at a
  // time through TileLender::Lend (LayoutDecoder.h), and learns the screen's
  // type and tile count only here: called once per decoded screen, after every
  // check has passed and before SetScreenTypes. Only meaningful for a declined
  // slot: a sink that lends the slot reads both from it. Optional.
  virtual void SetScreenShape(uint8_t index, uint8_t screenType, uint8_t tileCount) {
    (void) index;
    (void) screenType;
    (void) tileCount;
  }
  // Called once, after every check has passed, just before SetCounts: which
  // screens decoded as tile screens (bit k set = screen k has screenType != 0).
  // A sink that declines screen slots learns the screen types only here.
  // Optional: the default ignores it.
  virtual void SetScreenTypes(uint8_t tileMask) { (void) tileMask; }
  // Called once, after every check has passed and before SetVersion.
  virtual void SetCounts(uint8_t screens, bool hasDashboard, uint8_t dashboardScreenIndex) = 0;
  // Called last: the torn-read guard. A sink shared with another reader must
  // order this store after every other one (the firmware sink issues __DMB()).
  virtual void SetVersion(uint8_t version) = 0;

 protected:
  ~LayoutSink() = default;
};

/**
 * Decode one WATCHFACE_LAYOUT payload.
 *
 * Write mode: Open() is called only after `payload` parses as a CBOR map, so a
 * payload that is not a map leaves the target untouched. A non-Ok verdict means
 * the caller must abandon the target (the firmware aborts the seqlock window).
 *
 * Validate mode touches nothing the sink owns and never returns NoTarget; `sink`
 * is required by the signature only and none of its hooks is called.
 */
LayoutDecodeResult DecodeLayoutDocument(const uint8_t* payload, size_t len, LayoutSink& sink, LayoutDecodeMode mode);

/**
 * Sink over a resident WatchFaceLayout. The host full-struct target, and the
 * transitional firmware target (a derived class opens the seqlock window in
 * Open() and orders SetVersion). Open() does not reset: the unit does.
 */
class WatchFaceLayoutSink : public LayoutSink {
 public:
  explicit WatchFaceLayoutSink(LayoutService::WatchFaceLayout* layout = nullptr) : layout_(layout) {}

  bool Open() override { return layout_ != nullptr; }

  char* Text(LayoutText which) override {
    switch (which) {
      case LayoutText::TimeFormat:            return layout_->timeFormat.data();
      case LayoutText::DefaultActiveColor:    return layout_->defaultActiveColor.data();
      default:                                return layout_->defaultInactiveColor.data();
    }
  }

  LayoutService::WatchScreen* ScreenSlot(uint8_t index) override { return &layout_->screens[index]; }
  LayoutService::Dashboard* DashboardSlot() override { return &layout_->dashboard; }

  void SetCounts(uint8_t screens, bool hasDashboard, uint8_t dashboardScreenIndex) override {
    layout_->screenCount = screens;
    layout_->hasDashboard = hasDashboard;
    layout_->dashboardScreenIndex = dashboardScreenIndex;
  }

  void SetVersion(uint8_t version) override { layout_->version = version; }

 protected:
  LayoutService::WatchFaceLayout* layout_;
};

}
}
}
