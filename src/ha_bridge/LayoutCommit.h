#pragma once

// Layout commit logic (REQ009-FR-C8b, REQ009-FR-B5): validate, write, commit
// and abort of a WATCHFACE_LAYOUT push against a narrow store, plus the parse of
// the persisted record. Host-linkable: no littlefs, no SPI, no FS.cpp, no
// FreeRTOS timer calls. The only FreeRTOS calls are pvPortMalloc / vPortFree,
// which the host build satisfies with tools/fuzz/hoststubs/portable_stub.cpp.
//
// What runs where (the binding, LayoutStoreFlash and the resident step timer,
// is a separate item):
//
//   NimBLE host task, first chunk of a transfer
//       LayoutWriter::TryBeginTransfer()   phase Idle -> Transfer, or refuse
//                                          (before any allocation)
//   NimBLE host task, completed payload
//       LayoutWriter::Validate()           zero heap, no store access
//       LayoutWriter::HandOff(buf, len)    takes the reassembly buffer
//       ... then the caller arms the step timer. If arming fails it calls
//       LayoutWriter::CancelHandOff(), which releases the buffer and the phase.
//       On a rejected payload it calls AbortTransfer() instead.
//   Step timer (timer daemon), one Step() per expiry
//       Step() returns Continue: re-arm the timer (one tick). If the re-arm
//       command cannot be queued, call Step() again inline (the write is never
//       abandoned: an abandoned write strands an open handle and the buffer).
//       Committed / Failed: the write is over; the buffer, the context and the
//       phase are already released, in that order. On Committed the caller
//       publishes: __DMB(), layoutGen_ to even, then the update signal.
//       Idle: nothing was pending.
//
// The heap phase (Idle / Transfer / Activation) is one atomic byte the caller
// owns and injects. Transfer is held from the first chunk until the write
// releases the buffer; every materialization on the display task takes
// Activation by the same compare-exchange (3b.1 revision 3). No reader window
// exists: while Transfer is held nothing reads the store.
//
// Persisted record. One file: an 84 B header, then the dashboard section (if any
// screen is a dashboard), then one tile section per tile screen in screen
// order. Sections are raw images of ActiveScreenView::DashView and ::TileView,
// each guarded by a CRC32 in the header; the header is guarded by its own.
// A record that fails any check is "no layout" (REQ009-FR-B5(f)) and is never
// partially used.
//
// Format 2 (firmware 2.4.100) describes up to 6 screens. Format 1 (up to 4
// screens, a 72 B header) is still read, so a layout stored by an older build
// survives the update; only format 2 is written. ReadLayoutHeader hands back a
// format-1 header in the format-2 layout (see there).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "ActiveScreenView.h"
#include "LayoutDocumentDecoder.h"
#include "LayoutStore.h"

namespace Pinetime {
namespace Applications {
namespace HaBridge {

// ---------------------------------------------------------------------------
// Heap phase
// ---------------------------------------------------------------------------

enum class HeapPhase : uint8_t { Idle = 0, Transfer = 1, Activation = 2 };

using HeapPhaseCell = std::atomic<uint8_t>;

/** Compare-exchange Idle -> `phase`. False when the phase is not Idle. */
bool TryEnterHeapPhase(HeapPhaseCell& cell, HeapPhase phase);

/** Store Idle with release ordering, whoever holds the phase. */
void ReleaseHeapPhase(HeapPhaseCell& cell);

/**
 * Release the phase only if `expected` still holds it (compare-exchange
 * `expected` -> Idle, release ordering on success). False, and nothing changed,
 * when another holder has it: a side that ends its own hold cannot free the
 * other side's.
 */
bool ReleaseHeapPhaseFrom(HeapPhaseCell& cell, HeapPhase expected);

// ---------------------------------------------------------------------------
// CRC32 (ISO 3309 / ITU-T V.42), the nibble-table algorithm of
// BridgeController.cpp (Crc32Nibble, file-static there). 64 B in flash.
// ---------------------------------------------------------------------------

constexpr uint32_t kCrc32Init = 0xFFFFFFFFu;

/** Fold `len` bytes into a running state that started at kCrc32Init. */
uint32_t LayoutCrc32Update(uint32_t state, const uint8_t* data, size_t len);

inline uint32_t LayoutCrc32Final(uint32_t state) { return state ^ 0xFFFFFFFFu; }

inline uint32_t LayoutCrc32(const uint8_t* data, size_t len) {
  return LayoutCrc32Final(LayoutCrc32Update(kCrc32Init, data, len));
}

// ---------------------------------------------------------------------------
// The persisted record
// ---------------------------------------------------------------------------

constexpr uint32_t kLayoutRecordMagic = 0x31524C48u;  // "HLR1", little endian
constexpr uint8_t kLayoutRecordFormat = 2;
constexpr uint8_t kLayoutRecordFormatV1 = 1;  // read, never written
constexpr uint8_t kLayoutMaxScreens = LayoutService::kMaxScreens;
constexpr uint8_t kLayoutMaxScreensV1 = 4;

// Section indexes: 0 is the dashboard, 1 + k is the tile section of screen k.
constexpr uint8_t kSectionDashboard = 0;
constexpr uint8_t kSectionTile0 = 1;
constexpr uint8_t kLayoutSectionCount = 1 + kLayoutMaxScreens;
constexpr uint8_t kLayoutSectionCountV1 = 1 + kLayoutMaxScreensV1;
constexpr uint8_t kNoSection = 0xFF;

// Header `flags`: bit 0 = a dashboard section is present; bits 1..6 = screen k
// is a tile screen (a screen whose bit is clear is a dashboard screen); bit 7 is
// zero. Format 1 kept the tile bits at 4..7 and zero in bits 1..3.
constexpr uint8_t kFlagHasDashboard = 0x01;
constexpr uint8_t kFlagTileShift = 1;
constexpr uint8_t kFlagTileShiftV1 = 4;
static_assert(kFlagTileShift + kLayoutMaxScreens <= 8, "the header flags hold the dashboard bit and one bit per screen");

using ViewCommon = ActiveScreenView::Common;
using ViewDash = ActiveScreenView::DashView;
using ViewTile = ActiveScreenView::TileView;

struct LayoutRecordHeader {
  uint32_t magic;
  uint8_t format;
  uint8_t version;      // the layout version, 1..255
  uint8_t screenCount;  // 0..kLayoutMaxScreens
  uint8_t flags;
  // The type hash: the sizes of the images the sections hold. A record written
  // by a build whose views differ in size is rejected, not reinterpreted.
  uint16_t viewSize;    // sizeof(ActiveScreenView)
  uint16_t dashSize;    // sizeof(ActiveScreenView::DashView)
  uint16_t tileSize;    // sizeof(ActiveScreenView::TileView)
  uint16_t reserved0;   // zero
  // Image of ActiveScreenView::Common (screenType is zero here: it is a
  // property of the active screen, set by the materializer).
  alignas(4) uint8_t common[sizeof(ViewCommon)];
  uint16_t sectionOffset[kLayoutSectionCount];  // 0 = section absent
  uint16_t reserved1;   // zero
  uint32_t sectionCrc[kLayoutSectionCount];     // 0 for an absent section
  uint32_t headerCrc;   // CRC32 of every byte before this field
};

// The layout the parser and the writer both rely on. Any change to these
// numbers, to the view types or to the header is a format change: bump
// kLayoutRecordFormat with it.
static_assert(sizeof(LayoutRecordHeader) == 84, "layout record header is 84 B");
static_assert(sizeof(ViewCommon) == 20, "the header holds the 20 B Common block");
static_assert(offsetof(LayoutRecordHeader, magic) == 0, "header offset");
static_assert(offsetof(LayoutRecordHeader, format) == 4, "header offset");
static_assert(offsetof(LayoutRecordHeader, version) == 5, "header offset");
static_assert(offsetof(LayoutRecordHeader, screenCount) == 6, "header offset");
static_assert(offsetof(LayoutRecordHeader, flags) == 7, "header offset");
static_assert(offsetof(LayoutRecordHeader, viewSize) == 8, "header offset");
static_assert(offsetof(LayoutRecordHeader, dashSize) == 10, "header offset");
static_assert(offsetof(LayoutRecordHeader, tileSize) == 12, "header offset");
static_assert(offsetof(LayoutRecordHeader, reserved0) == 14, "header offset");
static_assert(offsetof(LayoutRecordHeader, common) == 16, "header offset");
static_assert(offsetof(LayoutRecordHeader, sectionOffset) == 36, "header offset");
static_assert(offsetof(LayoutRecordHeader, reserved1) == 50, "header offset");
static_assert(offsetof(LayoutRecordHeader, sectionCrc) == 52, "header offset");
static_assert(offsetof(LayoutRecordHeader, headerCrc) == 80, "header offset");

// The format-1 header, as it sits in a stored file: 5 section slots, so
// the offsets, the CRCs and the header CRC are earlier than in format 2. Fields
// up to and including `common` (bytes 0..35) are the same in both. It is never
// instantiated: ReadLayoutHeader reads a format-1 header over a
// LayoutRecordHeader and rewrites it in place, so the display task's stack does
// not carry a second header.
constexpr size_t kLayoutHeaderBytesV1 = 72;
constexpr size_t kV1SectionOffsetAt = 36;  // uint16_t[5]
constexpr size_t kV1Reserved1At = 46;      // uint16_t, zero
constexpr size_t kV1SectionCrcAt = 48;     // uint32_t[5]
constexpr size_t kV1HeaderCrcAt = 68;      // CRC32 of bytes 0..67
static_assert(kV1SectionOffsetAt + kLayoutSectionCountV1 * 2 == kV1Reserved1At, "format 1 header layout");
static_assert(kV1Reserved1At + 2 == kV1SectionCrcAt, "format 1 header layout");
static_assert(kV1SectionCrcAt + kLayoutSectionCountV1 * 4 == kV1HeaderCrcAt, "format 1 header layout");
static_assert(kV1HeaderCrcAt + 4 == kLayoutHeaderBytesV1, "format 1 header is 72 B");
// The tile section is streamed as 17 B of prefix plus the tiles, one at a time.
static_assert(offsetof(ViewTile, defaultActiveColor) == 0, "tile image layout");
static_assert(offsetof(ViewTile, defaultInactiveColor) == 8, "tile image layout");
static_assert(offsetof(ViewTile, tileCount) == 16, "tile image layout");
static_assert(offsetof(ViewTile, tiles) == 17, "tile image layout");
static_assert(sizeof(ViewTile) == 17 + LayoutService::kMaxTilesPerScreen * sizeof(LayoutService::WatchTile),
              "tile image is the prefix plus kMaxTilesPerScreen tiles");
static_assert(LayoutService::kMaxTilesPerScreen == 4, "the tile chunk loop assumes 4 tiles");
// The sizes the hash pins on the reference target (arm and host agree, the
// image holds only char, bool and small integers).
static_assert(sizeof(ActiveScreenView) == 484, "ActiveScreenView size changed: format change");
static_assert(sizeof(ViewDash) == 304, "DashView size changed: format change");
static_assert(sizeof(ViewTile) == 461, "TileView size changed: format change");

constexpr size_t kLayoutHeaderBytes = sizeof(LayoutRecordHeader);

/** The header's length in a record of `format` (72 B for format 1, 84 B otherwise). */
constexpr size_t LayoutHeaderBytesOf(uint8_t format) {
  return format == kLayoutRecordFormatV1 ? kLayoutHeaderBytesV1 : kLayoutHeaderBytes;
}
constexpr size_t kLayoutRecordMaxBytes =
  kLayoutHeaderBytes + sizeof(ViewDash) + kLayoutMaxScreens * sizeof(ViewTile);

/** The record's own section map, derived from the header's flags and count. */
struct LayoutSectionMap {
  uint16_t offset[kLayoutSectionCount];  // 0 = absent
  uint16_t length[kLayoutSectionCount];
  uint16_t total;                        // bytes in the record
};

/** `headerBytes`: the record's header length, LayoutHeaderBytesOf(its format). */
LayoutSectionMap ComputeLayoutSectionMap(uint8_t screenCount, uint8_t flags, size_t headerBytes = kLayoutHeaderBytes);

/** The section that holds screen `screen`'s data, or kNoSection. */
uint8_t SectionForScreen(const LayoutRecordHeader& header, uint8_t screen);

enum class RecordStatus : uint8_t {
  Ok,
  Absent,       // no record, or an empty one: REQ009-FR-B5(f) boot state
  ReadError,    // the store reported an I/O error
  Truncated,    // a read came back short
  BadMagic,
  BadFormat,
  BadTypeHash,  // the section image sizes differ from this build's
  BadHeader,    // header CRC, or a structural rule of the header
  BadSection    // a section CRC
};

/**
 * Read and check the header. On anything but Ok `out` must not be used. A
 * format-1 record's header comes back in the format-2 layout (`flags` with the
 * tile bits at 1..6, the section offsets and CRCs in their slots), with `format`
 * still 1 so the section map is computed with the 72 B header; its `headerCrc`
 * is not meaningful.
 */
RecordStatus ReadLayoutHeader(LayoutStore& store, LayoutRecordHeader& out);

/**
 * Read section `section` (kSectionDashboard, or kSectionTile0 + screen) into
 * `out`, checking its CRC. `cap` must hold the section's length; the length is
 * written to `*outLen` when it is not null. `header` must have come from
 * ReadLayoutHeader.
 */
RecordStatus ReadLayoutSection(LayoutStore& store, const LayoutRecordHeader& header, uint8_t section,
                               uint8_t* out, size_t cap, size_t* outLen = nullptr);

/**
 * Check the whole record: the header and the CRC of every present section, in
 * 64 B reads, with no allocation. What a boot-time check, and the record fuzz
 * target, run.
 */
RecordStatus ValidateLayoutRecord(LayoutStore& store);

// ---------------------------------------------------------------------------
// The write passes
// ---------------------------------------------------------------------------
//
// Each pass decodes the retained payload in Write mode through a sink that lends
// ONE decode target and declines every other (LayoutDocumentDecoder.h), so the
// slot a pass needs is the only large object on its stack frame:
//   dashboard pass  the Dashboard slot plus the three layout-level text
//                   slots it needs (time format, the two default colours)
//   tile pass       one WatchTile (111 B): the tiles are lent one at a time
// The slot lives in the frame of the pass function and is gone when it returns;
// nothing but the small results below survives a pass. The passes are declared
// here so the host tests can compare each with a full-document decode.

// A tile section is streamed: a 17 B prefix (the two default colours, the tile
// count), then the four tiles, each straight from the decoded slot. WatchTile is
// the very type TileView holds, so a tile is written as it sits in the slot.
constexpr size_t kTileBytes = sizeof(LayoutService::WatchTile);
constexpr size_t kTilePrefixBytes = 17;
static_assert(std::is_same<decltype(ViewTile::tiles)::value_type, LayoutService::WatchTile>::value,
              "TileView::tiles holds WatchTile: a tile is written from the slot unchanged");
static_assert(kTileBytes == 111, "WatchTile is 111 B with no padding");
static_assert(sizeof(ViewTile) == kTilePrefixBytes + LayoutService::kMaxTilesPerScreen * kTileBytes, "tile section = prefix + four tiles");

// The dashboard image is emitted in pieces of at most this many bytes (one store
// call each; the store's own 16 B cache takes them at the size it programs).
constexpr size_t kDashPieceBytes = 16;
constexpr uint8_t kDashPieces = static_cast<uint8_t>((sizeof(ViewDash) + kDashPieceBytes - 1) / kDashPieceBytes);

/** What the layout pass learns about the document. Valid when the verdict is Ok. */
struct LayoutPassInfo {
  uint8_t version;
  uint8_t screenCount;
  uint8_t tileMask;       // bit k set: screen k is a tile screen
  bool hasDashboard;
  char timeFormatLead;    // first character of timeFormat
  uint8_t colors[16];     // defaultActiveColor (8), defaultInactiveColor (8), after the unit's defaults
};

/** What the dashboard pass learns: the dashboard's part of the Common block, and the section CRC. */
struct LayoutDashFacts {
  uint32_t lastEnrichedAt;
  uint32_t eventStartEpoch[ActiveScreenView::kEventSlots];
  uint8_t eventCount;
  uint32_t dashCrc;       // CRC32 of the DashView image (0 when there is no dashboard screen)
};

/** What a tile pass learns about one screen. Valid when the verdict is Ok. */
struct LayoutTilePass {
  uint8_t screenType;
  uint8_t tileCount;
  uint32_t crc;           // CRC32 of the TileView image: prefix, then the four tiles
};

/**
 * The layout pass: lends three small text slots (time format, the two default
 * colours) and no screen or dashboard slot. Verdict and version are those of
 * DecodeLayoutDocument in Validate mode on the same bytes.
 */
LayoutDecodeResult DecodeLayoutPass(const uint8_t* payload, size_t len, LayoutPassInfo& out);

/**
 * The dashboard pass: lends the Dashboard slot only, declines everything else.
 * Same verdict as Validate.
 */
LayoutDecodeResult DecodeDashPass(const uint8_t* payload, size_t len, LayoutDashFacts& out);

/**
 * A tile pass over screen `screen`: lends that screen's tiles one at a time. `colors` is
 * the 16 B colour pair from the dashboard pass (it is part of the section
 * prefix and so of the CRC). Same verdict as Validate.
 */
LayoutDecodeResult DecodeTilePass(const uint8_t* payload, size_t len, uint8_t screen, const uint8_t* colors,
                                  LayoutTilePass& out);

/**
 * Copy tile `tile` (0..3) of screen `screen` into `out` (kTileBytes). `info`
 * (optional) receives the screen type and tile count.
 */
LayoutDecodeResult CopyTile(const uint8_t* payload, size_t len, uint8_t screen, uint8_t tile, uint8_t* out,
                            LayoutTilePass* info = nullptr);

/**
 * Copy piece `piece` (0..kDashPieces-1) of the DashView image into `out`
 * (kDashPieceBytes); `*outLen` gets its length. The image is exactly what a
 * DashView built from the decoded Dashboard holds, padding zero.
 */
LayoutDecodeResult CopyDashPiece(const uint8_t* payload, size_t len, uint8_t piece, uint8_t* out, size_t* outLen);

// ---------------------------------------------------------------------------
// The writer: the step machine
// ---------------------------------------------------------------------------

enum class StepResult : uint8_t {
  Idle,       // nothing was pending
  Continue,   // one step done: re-arm the step timer
  Committed,  // the record is durable; buffer, context and phase are released
  Failed      // the write failed and was aborted; the previous record stands
};

/**
 * The write's working state: allocated by the first Step, freed by the last.
 * One small pvPortMalloc block: no decode target is kept here (each pass keeps
 * its slot on the stack of the step that decodes).
 */
struct LayoutWriteContext {
  uint32_t sectionCrc[kLayoutSectionCount];
  uint32_t runCrc;
  uint8_t common[sizeof(ViewCommon)];
  uint8_t colors[16];
  uint8_t tileCount[kLayoutMaxScreens];
  uint8_t stage;
  uint8_t version;
  uint8_t screenCount;
  uint8_t flags;
  uint8_t section;  // the section being prepared or written
  uint8_t chunk;    // the piece or tile being written
  bool fileOpen;
};
static_assert(sizeof(LayoutWriteContext) <= 120, "3b.1 revision 2: the write context is a 128 B block (a request of at most 120 B)");

class LayoutWriter {
 public:
  LayoutWriter(LayoutStore& store, HeapPhaseCell& phase) : store_(store), phase_(phase) {}

  /**
   * First chunk of a transfer: take the heap phase (Idle -> Transfer). False
   * when the phase is held (another transfer, a pending or running write, a
   * materialization): the transfer is refused, and nothing has been allocated.
   */
  bool TryBeginTransfer();

  /** A transfer that ends before hand-off (rejected payload, reassembly timeout). */
  void AbortTransfer();

  /**
   * Validate a completed payload: the C8a Validate pass. Zero heap, no store
   * access, linear in the payload.
   */
  static LayoutDecodeResult Validate(const uint8_t* payload, size_t len);

  /**
   * Hand a validated payload to the writer. `buf` was allocated with
   * pvPortMalloc; on true the writer owns it and frees it exactly once, when
   * the write ends. False (the writer does not hold the Transfer phase, or a
   * write is already pending) leaves ownership with the caller.
   */
  bool HandOff(uint8_t* buf, size_t len);

  /**
   * The hand-off could not be armed: release the buffer and the phase. Only
   * valid before the first Step.
   */
  void CancelHandOff();

  /**
   * A pending write that never started (the system did not wake): release the
   * buffer and the phase without a store call, counted as failed. False, and
   * nothing done, once the first Step has run or when nothing is pending.
   */
  bool Abandon();

  /** One step: at most one store call. See the file comment for the contract. */
  StepResult Step();

  /** A push is pending or being written. */
  bool Busy() const { return state_.load(std::memory_order_acquire) != kIdle; }

  uint32_t committed() const { return committed_; }
  uint32_t failed() const { return failed_; }
  uint32_t refused() const { return refused_; }

 private:
  static constexpr uint8_t kIdle = 0;
  static constexpr uint8_t kPending = 1;
  static constexpr uint8_t kRunning = 2;

  bool StartWrite();
  StepResult StepOpen();
  StepResult StepClose();
  StepResult StepPrepLayout();
  StepResult StepPrepDash();
  StepResult StepPrepTile();
  StepResult StepHeader();
  StepResult StepDashPiece();
  StepResult StepTilePrefix();
  StepResult StepTileChunk();
  StepResult Fail();
  StepResult Succeed();
  void Release(bool ok);
  // Point the cursor at the first present tile section at or after `from`, in
  // stage `tileStage`, or at `endStage` when there is none.
  void SeekTile(uint8_t from, uint8_t tileStage, uint8_t endStage);

  LayoutStore& store_;
  HeapPhaseCell& phase_;
  uint8_t* buf_ {nullptr};
  size_t len_ {0};
  LayoutWriteContext* ctx_ {nullptr};
  std::atomic<uint8_t> state_ {kIdle};
  uint32_t committed_ {0};
  uint32_t failed_ {0};
  uint32_t refused_ {0};
};

}
}
}
