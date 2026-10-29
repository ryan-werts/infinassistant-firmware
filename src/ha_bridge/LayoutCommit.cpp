#include "ha_bridge/LayoutCommit.h"

#include <FreeRTOS.h>
#include <portable.h>  // pvPortMalloc / vPortFree, the same heap the reassembly buffer comes from

#include <algorithm>
#include <cstring>

// Layout commit logic (REQ009-FR-C8b). See the header for the contract and for
// what runs on which context.
//
// The write is a step machine so the timer daemon is never held for the whole
// write (3b.1 revision 2): prep (a dashboard pass, then one pass per tile
// screen, no store call), open, header, dashboard pieces, per tile section a
// prefix and four tiles (one tile per step), close. Each Step() makes at most one
// store call. A step that needs section data decodes the retained payload again
// in Write mode through a sink that lends only the one target the section needs;
// that slot lives on the daemon stack in the pass function and is gone when it
// returns. Nothing but the payload and a context of about 70 B outlives a step.

#define HA_NOINLINE __attribute__((noinline))

namespace Pinetime {
namespace Applications {
namespace HaBridge {

// ---------------------------------------------------------------------------
// Heap phase
// ---------------------------------------------------------------------------

bool TryEnterHeapPhase(HeapPhaseCell& cell, HeapPhase phase) {
  uint8_t expected = static_cast<uint8_t>(HeapPhase::Idle);
  return cell.compare_exchange_strong(expected, static_cast<uint8_t>(phase), std::memory_order_acq_rel,
                                      std::memory_order_acquire);
}

void ReleaseHeapPhase(HeapPhaseCell& cell) {
  cell.store(static_cast<uint8_t>(HeapPhase::Idle), std::memory_order_release);
}

// One copy: inlined into every write step (each ends in Release), the
// compare-exchange loop cost about 1.4 KB of flash and 8 B on the daemon chain.
__attribute__((noinline)) bool ReleaseHeapPhaseFrom(HeapPhaseCell& cell, HeapPhase expected) {
  uint8_t held = static_cast<uint8_t>(expected);
  return cell.compare_exchange_strong(held, static_cast<uint8_t>(HeapPhase::Idle), std::memory_order_acq_rel,
                                      std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// CRC32
// ---------------------------------------------------------------------------

uint32_t LayoutCrc32Update(uint32_t state, const uint8_t* data, size_t len) {
  static constexpr uint32_t kTable[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
    0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
    0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
  };
  uint32_t crc = state;
  for (size_t i = 0; i < len; i++) {
    crc = (crc >> 4) ^ kTable[(crc ^ data[i]) & 0x0F];
    crc = (crc >> 4) ^ kTable[(crc ^ (data[i] >> 4)) & 0x0F];
  }
  return crc;
}

// ---------------------------------------------------------------------------
// Record: section map and parser
// ---------------------------------------------------------------------------

LayoutSectionMap ComputeLayoutSectionMap(uint8_t screenCount, uint8_t flags, size_t headerBytes) {
  LayoutSectionMap m;
  std::memset(&m, 0, sizeof(m));
  uint16_t off = static_cast<uint16_t>(headerBytes);
  if ((flags & kFlagHasDashboard) != 0) {
    m.offset[kSectionDashboard] = off;
    m.length[kSectionDashboard] = static_cast<uint16_t>(sizeof(ViewDash));
    off = static_cast<uint16_t>(off + sizeof(ViewDash));
  }
  for (uint8_t k = 0; k < screenCount && k < kLayoutMaxScreens; ++k) {
    if ((flags & (1u << (kFlagTileShift + k))) == 0) continue;
    m.offset[kSectionTile0 + k] = off;
    m.length[kSectionTile0 + k] = static_cast<uint16_t>(sizeof(ViewTile));
    off = static_cast<uint16_t>(off + sizeof(ViewTile));
  }
  m.total = off;
  return m;
}

uint8_t SectionForScreen(const LayoutRecordHeader& header, uint8_t screen) {
  if (screen >= header.screenCount || screen >= kLayoutMaxScreens) return kNoSection;
  if ((header.flags & (1u << (kFlagTileShift + screen))) != 0) return static_cast<uint8_t>(kSectionTile0 + screen);
  return (header.flags & kFlagHasDashboard) != 0 ? kSectionDashboard : kNoSection;
}

namespace {

// The structural rules of a header whose CRC has already matched. The writer
// only ever produces canonical headers, so a violation means a foreign or buggy
// writer: reject it rather than trust offsets it computed.
bool HeaderIsCanonical(const LayoutRecordHeader& h) {
  if (h.reserved0 != 0 || h.reserved1 != 0) return false;
  if (h.version == 0 || h.screenCount > kLayoutMaxScreens) return false;
  if ((h.flags >> (kFlagTileShift + kLayoutMaxScreens)) != 0) return false;  // no bit above the last screen's

  const uint8_t tileMask = static_cast<uint8_t>(h.flags >> kFlagTileShift);
  const uint8_t fullMask = static_cast<uint8_t>((1u << h.screenCount) - 1u);
  if ((tileMask & ~fullMask) != 0) return false;
  const bool hasDash = (h.flags & kFlagHasDashboard) != 0;
  if (hasDash != (tileMask != fullMask)) return false;

  const LayoutSectionMap map = ComputeLayoutSectionMap(h.screenCount, h.flags, LayoutHeaderBytesOf(h.format));
  for (uint8_t s = 0; s < kLayoutSectionCount; ++s) {
    if (h.sectionOffset[s] != map.offset[s]) return false;
    if (map.offset[s] == 0 && h.sectionCrc[s] != 0) return false;
  }

  ViewCommon c;
  std::memcpy(&c, h.common, sizeof(c));
  if (c.version != h.version || c.screenCount != h.screenCount) return false;
  if (c.screenType != 0 || c.eventCount > ActiveScreenView::kEventSlots) return false;
  return true;
}

// A format-1 header, read over `h` and already CRC-checked, rewritten in place as
// a format-2 one: the tile bits of `flags` move from 4..7 to 1..4, and the section
// offsets and CRCs go to their format-2 slots (slots 5 and 6 are absent). Fields
// up to `common` are the same in both formats. False when a format-1 rule fails
// (what the format-1 reader rejected: more than 4 screens, a flag in bits 1..3, a
// non-zero reserved word).
bool UpgradeV1Header(LayoutRecordHeader& h) {
  uint8_t* raw = reinterpret_cast<uint8_t*>(&h);
  uint16_t offsets[kLayoutSectionCountV1];
  uint32_t crcs[kLayoutSectionCountV1];
  uint16_t reserved1;
  std::memcpy(offsets, raw + kV1SectionOffsetAt, sizeof(offsets));
  std::memcpy(&reserved1, raw + kV1Reserved1At, sizeof(reserved1));
  std::memcpy(crcs, raw + kV1SectionCrcAt, sizeof(crcs));
  if (reserved1 != 0 || h.screenCount > kLayoutMaxScreensV1 || (h.flags & 0x0E) != 0) return false;

  const uint8_t v1Flags = h.flags;
  std::memset(raw + kV1SectionOffsetAt, 0, kLayoutHeaderBytes - kV1SectionOffsetAt);  // also clears the format-1 bytes past the header
  h.flags = static_cast<uint8_t>((v1Flags & kFlagHasDashboard) | ((v1Flags >> kFlagTileShiftV1) << kFlagTileShift));
  for (uint8_t s = 0; s < kLayoutSectionCountV1; ++s) {
    h.sectionOffset[s] = offsets[s];
    h.sectionCrc[s] = crcs[s];
  }
  return true;
}

}  // namespace

RecordStatus ReadLayoutHeader(LayoutStore& store, LayoutRecordHeader& out) {
  std::memset(&out, 0, sizeof(out));
  // Read a format-2 header's worth: a format-1 file's header is the first 72 B of it.
  const int got = store.ReadAt(0, reinterpret_cast<uint8_t*>(&out), sizeof(out));
  if (got < 0) return RecordStatus::ReadError;
  if (got == 0) return RecordStatus::Absent;
  if (static_cast<size_t>(got) < kLayoutHeaderBytesV1) return RecordStatus::Truncated;

  if (out.magic != kLayoutRecordMagic) return RecordStatus::BadMagic;
  const bool v1 = out.format == kLayoutRecordFormatV1;
  if (!v1 && out.format != kLayoutRecordFormat) return RecordStatus::BadFormat;
  if (!v1 && static_cast<size_t>(got) < sizeof(out)) return RecordStatus::Truncated;
  if (out.viewSize != sizeof(ActiveScreenView) || out.dashSize != sizeof(ViewDash) || out.tileSize != sizeof(ViewTile)) {
    return RecordStatus::BadTypeHash;
  }

  const uint8_t* raw = reinterpret_cast<const uint8_t*>(&out);
  uint32_t storedCrc;
  size_t crcAt;
  if (v1) {
    crcAt = kV1HeaderCrcAt;
  } else {
    crcAt = offsetof(LayoutRecordHeader, headerCrc);
  }
  std::memcpy(&storedCrc, raw + crcAt, sizeof(storedCrc));
  if (LayoutCrc32(raw, crcAt) != storedCrc) return RecordStatus::BadHeader;

  if (v1 && !UpgradeV1Header(out)) return RecordStatus::BadHeader;
  if (!HeaderIsCanonical(out)) return RecordStatus::BadHeader;
  return RecordStatus::Ok;
}

RecordStatus ReadLayoutSection(LayoutStore& store, const LayoutRecordHeader& header, uint8_t section, uint8_t* out,
                               size_t cap, size_t* outLen) {
  if (section >= kLayoutSectionCount) return RecordStatus::BadSection;
  const LayoutSectionMap map = ComputeLayoutSectionMap(header.screenCount, header.flags, LayoutHeaderBytesOf(header.format));
  if (map.offset[section] == 0 || cap < map.length[section]) return RecordStatus::BadSection;

  const size_t len = map.length[section];
  const int got = store.ReadAt(map.offset[section], out, len);
  if (got < 0) return RecordStatus::ReadError;
  if (static_cast<size_t>(got) < len) return RecordStatus::Truncated;
  if (LayoutCrc32(out, len) != header.sectionCrc[section]) return RecordStatus::BadSection;
  if (outLen != nullptr) *outLen = len;
  return RecordStatus::Ok;
}

RecordStatus ValidateLayoutRecord(LayoutStore& store) {
  LayoutRecordHeader h;
  RecordStatus rs = ReadLayoutHeader(store, h);
  if (rs != RecordStatus::Ok) return rs;

  const LayoutSectionMap map = ComputeLayoutSectionMap(h.screenCount, h.flags, LayoutHeaderBytesOf(h.format));
  uint8_t chunk[64];
  for (uint8_t s = 0; s < kLayoutSectionCount; ++s) {
    if (map.offset[s] == 0) continue;
    uint32_t crc = kCrc32Init;
    size_t done = 0;
    while (done < map.length[s]) {
      const size_t n = std::min(sizeof(chunk), static_cast<size_t>(map.length[s]) - done);
      const int got = store.ReadAt(map.offset[s] + done, chunk, n);
      if (got < 0) return RecordStatus::ReadError;
      if (static_cast<size_t>(got) < n) return RecordStatus::Truncated;
      crc = LayoutCrc32Update(crc, chunk, n);
      done += n;
    }
    if (LayoutCrc32Final(crc) != h.sectionCrc[s]) return RecordStatus::BadSection;
  }
  return RecordStatus::Ok;
}

// ---------------------------------------------------------------------------
// The write passes
// ---------------------------------------------------------------------------

namespace {

// The C8a sinks of the passes. Each lends at most one target and declines all
// the rest with nullptr; the unit walks a declined target through its shadow
// counters (LayoutDocumentDecoder.h), so the verdict and the lent target are what
// a full sink would get. They are kept as small as they can be: a pass function
// holds its slot and its sink in one frame, and that frame is on the daemon stack.
class DeclineSink : public LayoutSink {
 public:
  bool Open() override { return true; }
  char* Text(LayoutText) override { return nullptr; }
  LayoutService::WatchScreen* ScreenSlot(uint8_t) override { return nullptr; }
  LayoutService::Dashboard* DashboardSlot() override { return nullptr; }
  void SetCounts(uint8_t, bool, uint8_t) override {}
  void SetVersion(uint8_t) override {}
};

// The tile sinks lend no screen slot: they lend the tiles of one screen through
// TileLender (LayoutDecoder.h), so a tile pass holds one WatchTile (111 B) and
// not the whole WatchScreen (447 B). The screen's type and tile count come back
// through SetScreenShape.
class TileShapeSink : public DeclineSink {
 public:
  void SetScreenShape(uint8_t index, uint8_t screenType, uint8_t tileCount) override {
    if (index == screen_) {
      screenType_ = screenType;
      tileCount_ = tileCount;
    }
  }
  uint8_t screenType() const { return screenType_; }
  uint8_t tileCount() const { return tileCount_; }

 protected:
  explicit TileShapeSink(uint8_t screen) : screen_(screen) {}
  // A tile starts zeroed, exactly as the unit zeroes a screen slot.
  static void ZeroTile(LayoutService::WatchTile* t) { std::memset(static_cast<void*>(t), 0, sizeof(*t)); }
  const uint8_t screen_;

 private:
  uint8_t screenType_ {0};
  uint8_t tileCount_ {0};
};

// Lends the wanted tile of the wanted screen and no other.
class OneTileSink final : public TileShapeSink {
 public:
  OneTileSink(uint8_t screen, uint8_t tile) : TileShapeSink(screen), tile_(tile) { ZeroTile(&slot_); }
  LayoutService::WatchTile* Lend(uint8_t screen, uint8_t tile) override {
    return screen == screen_ && tile == tile_ ? &slot_ : nullptr;
  }
  const LayoutService::WatchTile& slot() const { return slot_; }

 private:
  const uint8_t tile_;
  LayoutService::WatchTile slot_;
};

// Lends the tiles of the wanted screen one at a time, in order, and folds each
// into a CRC state as the walk moves on: tile j is final once the walk asks for
// a later one (or the decode ends), and a tile never asked for is all zero, as
// it is in a whole-screen slot. The state starts at 0, not at kCrc32Init, so the
// section prefix, which is only known after the walk (it carries the tile count),
// can be put in front afterwards (TileSectionCrc).
class TileCrcSink final : public TileShapeSink {
 public:
  explicit TileCrcSink(uint8_t screen) : TileShapeSink(screen) { ZeroTile(&cur_); }
  LayoutService::WatchTile* Lend(uint8_t screen, uint8_t tile) override {
    if (screen != screen_) return nullptr;
    while (index_ < tile) Advance();
    return &cur_;
  }
  // CRC state of the four tiles' bytes from a zero start.
  uint32_t Finish() {
    while (index_ < LayoutService::kMaxTilesPerScreen) Advance();
    return state_;
  }

 private:
  void Advance() {
    state_ = LayoutCrc32Update(state_, reinterpret_cast<const uint8_t*>(&cur_), kTileBytes);
    ZeroTile(&cur_);
    ++index_;
  }
  uint8_t index_ {0};
  uint32_t state_ {0};
  LayoutService::WatchTile cur_;
};

// CRC state after `prefix` then the four tiles, from the tiles' zero-start
// state. The update is linear in the state, so
//   update(s, tiles) = update(s, four zero tiles) ^ update(0, tiles).
uint32_t TileSectionCrc(const uint8_t* prefix, uint32_t tilesState) {
  static constexpr uint8_t kZeroTile[kTileBytes] = {};
  uint32_t s = LayoutCrc32Update(kCrc32Init, prefix, kTilePrefixBytes);
  for (size_t j = 0; j < LayoutService::kMaxTilesPerScreen; ++j) s = LayoutCrc32Update(s, kZeroTile, kTileBytes);
  return s ^ tilesState;
}

// Lends the Dashboard slot, and notes whether the layout has a dashboard screen.
class DashSink final : public DeclineSink {
 public:
  explicit DashSink(LayoutService::Dashboard* slot) : slot_(slot) {}
  LayoutService::Dashboard* DashboardSlot() override { return slot_; }
  void SetCounts(uint8_t, bool hasDashboard, uint8_t) override { hasDash_ = hasDashboard; }
  bool hasDash() const { return hasDash_; }

 private:
  LayoutService::Dashboard* slot_;
  bool hasDash_ {false};
};

// Lends the three layout-level text slots the writer needs, and notes the
// layout's counts and screen types. No slot of any size.
class LayoutOnlySink final : public DeclineSink {
 public:
  LayoutOnlySink(char* timeFormat, char* active, char* inactive)
    : timeFormat_(timeFormat), active_(active), inactive_(inactive) {}
  char* Text(LayoutText which) override {
    switch (which) {
      case LayoutText::TimeFormat:           return timeFormat_;
      case LayoutText::DefaultActiveColor:   return active_;
      case LayoutText::DefaultInactiveColor: return inactive_;
      default:                               return nullptr;
    }
  }
  void SetScreenTypes(uint8_t tileMask) override { tileMask_ = tileMask; }
  void SetCounts(uint8_t screens, bool hasDashboard, uint8_t) override {
    screens_ = screens;
    hasDash_ = hasDashboard;
  }
  uint8_t screens() const { return screens_; }
  uint8_t tileMask() const { return tileMask_; }
  bool hasDash() const { return hasDash_; }

 private:
  char* timeFormat_;
  char* active_;
  char* inactive_;
  uint8_t tileMask_ {0};
  uint8_t screens_ {0};
  bool hasDash_ {false};
};

// The DashView image as a stream of byte spans in image order, padding as zero
// spans. The image is never built: a caller folds the spans into a CRC or copies
// the window of them it needs. Members are listed in DashView declaration order;
// the host test compares the stream with a DashView built from the same
// Dashboard, byte for byte.
template <typename F>
void EmitDashImage(const LayoutService::Dashboard& d, F&& emit) {
  static constexpr uint8_t kZeros[16] = {};
  static constexpr uint8_t kTrue = 1;
  size_t pos = 0;
  auto put = [&](size_t off, const void* src, size_t n) {
    while (pos < off) {
      const size_t z = std::min<size_t>(off - pos, sizeof(kZeros));
      emit(kZeros, z);
      pos += z;
    }
    emit(static_cast<const uint8_t*>(src), n);
    pos = off + n;
  };
  using Ev = ActiveScreenView::DashEvent;
  using Sg = ActiveScreenView::DashStatusGroup;
  using Se = ActiveScreenView::DashSensorGroup;

  put(offsetof(ViewDash, hasDashboard), &kTrue, 1);
  for (size_t i = 0; i < ActiveScreenView::kEventSlots; ++i) {
    const size_t base = offsetof(ViewDash, events) + i * sizeof(Ev);
    put(base + offsetof(Ev, time), d.events[i].time.data(), sizeof(d.events[i].time));
    put(base + offsetof(Ev, title), d.events[i].title.data(), sizeof(d.events[i].title));
  }
  put(offsetof(ViewDash, configurableValue), &d.configurableValue, sizeof(d.configurableValue));
  put(offsetof(ViewDash, inlineMetrics), d.inlineMetrics.data(), sizeof(d.inlineMetrics));
  put(offsetof(ViewDash, weatherCondition), d.weatherCondition.data(), sizeof(d.weatherCondition));
  put(offsetof(ViewDash, weatherHi), &d.weatherHi, 1);
  put(offsetof(ViewDash, weatherLo), &d.weatherLo, 1);
  put(offsetof(ViewDash, statusGroupCount), &d.statusGroupCount, 1);
  for (size_t g = 0; g < ActiveScreenView::kStatusGroups; ++g) {
    const size_t base = offsetof(ViewDash, statusGroups) + g * sizeof(Sg);
    put(base + offsetof(Sg, glyphs), d.statusGroups[g].glyphs.data(), sizeof(d.statusGroups[g].glyphs));
    put(base + offsetof(Sg, glyphCount), &d.statusGroups[g].glyphCount, 1);
  }
  put(offsetof(ViewDash, sensorGroupCount), &d.sensorGroupCount, 1);
  for (size_t g = 0; g < ActiveScreenView::kSensorGroups; ++g) {
    const size_t base = offsetof(ViewDash, sensorGroups) + g * sizeof(Se);
    put(base + offsetof(Se, values), d.sensorGroups[g].values.data(), sizeof(d.sensorGroups[g].values));
    put(base + offsetof(Se, valueCount), &d.sensorGroups[g].valueCount, 1);
  }
  while (pos < sizeof(ViewDash)) {
    const size_t z = std::min<size_t>(sizeof(ViewDash) - pos, sizeof(kZeros));
    emit(kZeros, z);
    pos += z;
  }
}

void BuildTilePrefix(const uint8_t* colors, uint8_t tileCount, uint8_t* out) {
  std::memcpy(out, colors, 16);
  out[16] = tileCount;
}

}  // namespace

// Each pass function owns its slot: it is on this frame and nowhere else, and it
// is gone when the function returns. Marked noinline so a caller's frame never
// absorbs it.

// The layout level: version, screen count and types, the time format lead and the
// colour pair. Lends only three small text slots.
HA_NOINLINE LayoutDecodeResult DecodeLayoutPass(const uint8_t* payload, size_t len, LayoutPassInfo& out) {
  char timeFormat[16] = {};
  char active[8] = {};
  char inactive[8] = {};
  LayoutOnlySink sink(timeFormat, active, inactive);
  const LayoutDecodeResult r = DecodeLayoutDocument(payload, len, sink, LayoutDecodeMode::Write);
  std::memset(&out, 0, sizeof(out));
  if (r.verdict != LayoutVerdict::Ok) return r;
  out.version = static_cast<uint8_t>(r.version);
  out.screenCount = sink.screens();
  out.tileMask = sink.tileMask();
  out.hasDashboard = sink.hasDash();
  out.timeFormatLead = timeFormat[0];
  std::memcpy(out.colors, active, 8);
  std::memcpy(out.colors + 8, inactive, 8);
  return r;
}

HA_NOINLINE LayoutDecodeResult DecodeDashPass(const uint8_t* payload, size_t len, LayoutDashFacts& out) {
  LayoutService::Dashboard dash;
  DashSink sink(&dash);
  const LayoutDecodeResult r = DecodeLayoutDocument(payload, len, sink, LayoutDecodeMode::Write);
  std::memset(&out, 0, sizeof(out));
  if (r.verdict != LayoutVerdict::Ok) return r;

  out.lastEnrichedAt = dash.lastEnrichedAt;
  for (size_t i = 0; i < ActiveScreenView::kEventSlots; ++i) out.eventStartEpoch[i] = dash.events[i].eventStartEpoch;
  out.eventCount = std::min<uint8_t>(dash.eventCount, static_cast<uint8_t>(ActiveScreenView::kEventSlots));
  if (sink.hasDash()) {
    uint32_t crc = kCrc32Init;
    EmitDashImage(dash, [&crc](const uint8_t* p, size_t n) { crc = LayoutCrc32Update(crc, p, n); });
    out.dashCrc = LayoutCrc32Final(crc);
  }
  return r;
}

HA_NOINLINE LayoutDecodeResult DecodeTilePass(const uint8_t* payload, size_t len, uint8_t screen, const uint8_t* colors,
                                              LayoutTilePass& out) {
  TileCrcSink sink(screen);
  const LayoutDecodeResult r = DecodeLayoutDocument(payload, len, sink, LayoutDecodeMode::Write);
  std::memset(&out, 0, sizeof(out));
  if (r.verdict != LayoutVerdict::Ok) return r;

  out.screenType = sink.screenType();
  out.tileCount = std::min<uint8_t>(sink.tileCount(), LayoutService::kMaxTilesPerScreen);
  uint8_t prefix[kTilePrefixBytes];
  BuildTilePrefix(colors, out.tileCount, prefix);
  out.crc = LayoutCrc32Final(TileSectionCrc(prefix, sink.Finish()));
  return r;
}

HA_NOINLINE LayoutDecodeResult CopyTile(const uint8_t* payload, size_t len, uint8_t screen, uint8_t tile, uint8_t* out,
                                        LayoutTilePass* info) {
  OneTileSink sink(screen, tile);
  const LayoutDecodeResult r = DecodeLayoutDocument(payload, len, sink, LayoutDecodeMode::Write);
  if (r.verdict != LayoutVerdict::Ok || tile >= LayoutService::kMaxTilesPerScreen) return r;
  std::memcpy(out, &sink.slot(), kTileBytes);
  if (info != nullptr) {
    info->screenType = sink.screenType();
    info->tileCount = std::min<uint8_t>(sink.tileCount(), LayoutService::kMaxTilesPerScreen);
    info->crc = 0;
  }
  return r;
}

HA_NOINLINE LayoutDecodeResult CopyDashPiece(const uint8_t* payload, size_t len, uint8_t piece, uint8_t* out,
                                             size_t* outLen) {
  LayoutService::Dashboard dash;
  DashSink sink(&dash);
  const LayoutDecodeResult r = DecodeLayoutDocument(payload, len, sink, LayoutDecodeMode::Write);
  if (r.verdict != LayoutVerdict::Ok || piece >= kDashPieces) return r;

  const size_t begin = static_cast<size_t>(piece) * kDashPieceBytes;
  const size_t end = std::min(begin + kDashPieceBytes, sizeof(ViewDash));
  size_t at = 0;  // position in the image of the next span
  EmitDashImage(dash, [&](const uint8_t* p, size_t n) {
    const size_t lo = std::max(at, begin);
    const size_t hi = std::min(at + n, end);
    if (lo < hi) std::memcpy(out + (lo - begin), p + (lo - at), hi - lo);
    at += n;
  });
  *outLen = end - begin;
  return r;
}

// ---------------------------------------------------------------------------
// Section map helpers
// ---------------------------------------------------------------------------

namespace {

enum Stage : uint8_t { kPrepLayout, kPrepDash, kPrepTile, kOpen, kHeader, kDashPiece, kTilePrefix, kTileChunk, kClose };

// The header image, from the context: the Common block and section CRCs the
// prep steps collected, the layout facts, the type hash.
void BuildHeader(const LayoutWriteContext& c, LayoutRecordHeader& h) {
  std::memset(&h, 0, sizeof(h));
  h.magic = kLayoutRecordMagic;
  h.format = kLayoutRecordFormat;
  h.version = c.version;
  h.screenCount = c.screenCount;
  h.flags = c.flags;
  h.viewSize = static_cast<uint16_t>(sizeof(ActiveScreenView));
  h.dashSize = static_cast<uint16_t>(sizeof(ViewDash));
  h.tileSize = static_cast<uint16_t>(sizeof(ViewTile));
  std::memcpy(h.common, c.common, sizeof(h.common));

  const LayoutSectionMap map = ComputeLayoutSectionMap(c.screenCount, c.flags);
  for (uint8_t s = 0; s < kLayoutSectionCount; ++s) {
    h.sectionOffset[s] = map.offset[s];
    h.sectionCrc[s] = map.offset[s] != 0 ? c.sectionCrc[s] : 0;
  }
  h.headerCrc = LayoutCrc32(reinterpret_cast<const uint8_t*>(&h), offsetof(LayoutRecordHeader, headerCrc));
}

bool HasTileSection(const LayoutWriteContext& c, uint8_t s) {
  const uint8_t k = static_cast<uint8_t>(s - kSectionTile0);
  return s >= kSectionTile0 && k < c.screenCount && (c.flags & (1u << (kFlagTileShift + k))) != 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// The writer
// ---------------------------------------------------------------------------

bool LayoutWriter::TryBeginTransfer() {
  if (TryEnterHeapPhase(phase_, HeapPhase::Transfer)) return true;
  ++refused_;
  return false;
}

void LayoutWriter::AbortTransfer() { ReleaseHeapPhaseFrom(phase_, HeapPhase::Transfer); }

LayoutDecodeResult LayoutWriter::Validate(const uint8_t* payload, size_t len) {
  WatchFaceLayoutSink sink(nullptr);
  return DecodeLayoutDocument(payload, len, sink, LayoutDecodeMode::Validate);
}

bool LayoutWriter::HandOff(uint8_t* buf, size_t len) {
  if (buf == nullptr || phase_.load(std::memory_order_acquire) != static_cast<uint8_t>(HeapPhase::Transfer)) return false;
  if (state_.load(std::memory_order_acquire) != kIdle) return false;
  buf_ = buf;
  len_ = len;
  state_.store(kPending, std::memory_order_release);
  return true;
}

void LayoutWriter::CancelHandOff() {
  if (state_.load(std::memory_order_acquire) != kPending) return;
  // A cancelled hand-off is a refusal, which the caller counts; it is not a
  // failed write, so no counter moves here.
  vPortFree(buf_);
  buf_ = nullptr;
  len_ = 0;
  state_.store(kIdle, std::memory_order_release);
  ReleaseHeapPhaseFrom(phase_, HeapPhase::Transfer);
}

// No store call has been made while pending, so ctx_ is null and Release frees
// only the buffer.
bool LayoutWriter::Abandon() {
  if (state_.load(std::memory_order_acquire) != kPending) return false;
  Release(false);
  return true;
}

// Free the buffer and the context, then hand the phase back. The order is the
// publish order of 3b.1 (d): buffer released, phase Idle, and only then can the
// caller signal.
void LayoutWriter::Release(bool ok) {
  if (ctx_ != nullptr) {
    vPortFree(ctx_);
    ctx_ = nullptr;
  }
  if (buf_ != nullptr) {
    vPortFree(buf_);
    buf_ = nullptr;
  }
  len_ = 0;
  if (ok) {
    ++committed_;
  } else {
    ++failed_;
  }
  state_.store(kIdle, std::memory_order_release);
  ReleaseHeapPhaseFrom(phase_, HeapPhase::Transfer);
}

StepResult LayoutWriter::Fail() {
  // Close an open handle without committing: the previous record stands.
  if (ctx_ != nullptr && ctx_->fileOpen) {
    store_.Abort();
    ctx_->fileOpen = false;
  }
  Release(false);
  return StepResult::Failed;
}

StepResult LayoutWriter::Succeed() {
  Release(true);
  return StepResult::Committed;
}

void LayoutWriter::SeekTile(uint8_t from, uint8_t tileStage, uint8_t endStage) {
  LayoutWriteContext& c = *ctx_;
  for (uint8_t s = from; s < kLayoutSectionCount; ++s) {
    if (!HasTileSection(c, s)) continue;
    c.section = s;
    c.chunk = 0;
    c.stage = tileStage;
    return;
  }
  c.stage = endStage;
}

// The steps. Each is its own non-inlined function with a small frame: the large
// object of a step is the decode slot, and that lives in the pass function the
// step calls (DecodeDashPass, DecodeTilePass, CopyTile, CopyDashPiece), so the
// chain of a step is its frame, the pass frame and the decoder, never a slot
// plus a store call. The store is called only from these small frames, after the
// pass has returned.

// Prep, layout pass: version, screen count and types, the colour pair, the time
// format lead. Starts the Common image. No store call.
HA_NOINLINE StepResult LayoutWriter::StepPrepLayout() {
  LayoutWriteContext& c = *ctx_;
  LayoutPassInfo p;
  const LayoutDecodeResult r = DecodeLayoutPass(buf_, len_, p);
  if (r.verdict != LayoutVerdict::Ok || r.version < 1 || r.version > 255) return Fail();
  if (p.screenCount > kLayoutMaxScreens || p.tileMask >= (1u << kLayoutMaxScreens)) return Fail();

  c.version = p.version;
  c.screenCount = p.screenCount;
  c.flags = static_cast<uint8_t>((p.hasDashboard ? kFlagHasDashboard : 0) | (p.tileMask << kFlagTileShift));
  std::memcpy(c.colors, p.colors, sizeof(c.colors));
  ViewCommon& common = *reinterpret_cast<ViewCommon*>(c.common);
  common.version = p.version;
  common.screenCount = p.screenCount;
  common.screenType = 0;
  common.timeFormatLead = p.timeFormatLead;
  c.stage = kPrepDash;
  return StepResult::Continue;
}

// Prep, dashboard pass: the dashboard's part of the Common block and, when the
// layout has a dashboard, the CRC of its section. No store call.
HA_NOINLINE StepResult LayoutWriter::StepPrepDash() {
  LayoutWriteContext& c = *ctx_;
  LayoutDashFacts f;
  const LayoutDecodeResult r = DecodeDashPass(buf_, len_, f);
  if (r.verdict != LayoutVerdict::Ok) return Fail();

  ViewCommon& common = *reinterpret_cast<ViewCommon*>(c.common);
  common.lastEnrichedAt = f.lastEnrichedAt;
  for (size_t i = 0; i < ActiveScreenView::kEventSlots; ++i) common.eventStartEpoch[i] = f.eventStartEpoch[i];
  common.eventCount = f.eventCount;
  if ((c.flags & kFlagHasDashboard) != 0) c.sectionCrc[kSectionDashboard] = f.dashCrc;
  SeekTile(kSectionTile0, kPrepTile, kOpen);
  return StepResult::Continue;
}

// Prep, one tile screen: its tile count and the CRC of its section. No store call.
HA_NOINLINE StepResult LayoutWriter::StepPrepTile() {
  LayoutWriteContext& c = *ctx_;
  const uint8_t screen = static_cast<uint8_t>(c.section - kSectionTile0);
  LayoutTilePass t;
  const LayoutDecodeResult r = DecodeTilePass(buf_, len_, screen, c.colors, t);
  if (r.verdict != LayoutVerdict::Ok || t.screenType == 0) return Fail();
  c.tileCount[screen] = t.tileCount;
  c.sectionCrc[c.section] = t.crc;
  SeekTile(static_cast<uint8_t>(c.section + 1), kPrepTile, kOpen);
  return StepResult::Continue;
}

// The first section to write after the header: the dashboard, else the first
// tile section, else the close.
HA_NOINLINE StepResult LayoutWriter::StepHeader() {
  LayoutWriteContext& c = *ctx_;
  LayoutRecordHeader h;
  BuildHeader(c, h);
  if (!store_.Write(reinterpret_cast<const uint8_t*>(&h), sizeof(h))) return Fail();
  if ((c.flags & kFlagHasDashboard) != 0) {
    c.section = kSectionDashboard;
    c.chunk = 0;
    c.runCrc = kCrc32Init;
    c.stage = kDashPiece;
  } else {
    SeekTile(kSectionTile0, kTilePrefix, kClose);
  }
  return StepResult::Continue;
}

HA_NOINLINE StepResult LayoutWriter::StepDashPiece() {
  LayoutWriteContext& c = *ctx_;
  uint8_t piece[kDashPieceBytes];
  size_t n = 0;
  const LayoutDecodeResult r = CopyDashPiece(buf_, len_, c.chunk, piece, &n);
  if (r.verdict != LayoutVerdict::Ok || n == 0) return Fail();
  c.runCrc = LayoutCrc32Update(c.runCrc, piece, n);
  if (!store_.Write(piece, n)) return Fail();
  if (++c.chunk < kDashPieces) return StepResult::Continue;
  if (LayoutCrc32Final(c.runCrc) != c.sectionCrc[kSectionDashboard]) return Fail();
  SeekTile(kSectionTile0, kTilePrefix, kClose);
  return StepResult::Continue;
}

// The 17 B prefix of a tile section: no decode, everything is in the context.
HA_NOINLINE StepResult LayoutWriter::StepTilePrefix() {
  LayoutWriteContext& c = *ctx_;
  uint8_t prefix[kTilePrefixBytes];
  BuildTilePrefix(c.colors, c.tileCount[c.section - kSectionTile0], prefix);
  c.runCrc = LayoutCrc32Update(kCrc32Init, prefix, sizeof(prefix));
  if (!store_.Write(prefix, sizeof(prefix))) return Fail();
  c.chunk = 0;
  c.stage = kTileChunk;
  return StepResult::Continue;
}

HA_NOINLINE StepResult LayoutWriter::StepTileChunk() {
  LayoutWriteContext& c = *ctx_;
  uint8_t tile[kTileBytes];
  LayoutTilePass info;
  const LayoutDecodeResult r =
    CopyTile(buf_, len_, static_cast<uint8_t>(c.section - kSectionTile0), c.chunk, tile, &info);
  if (r.verdict != LayoutVerdict::Ok || info.screenType == 0) return Fail();
  c.runCrc = LayoutCrc32Update(c.runCrc, tile, sizeof(tile));
  if (!store_.Write(tile, sizeof(tile))) return Fail();
  if (++c.chunk < LayoutService::kMaxTilesPerScreen) return StepResult::Continue;
  if (LayoutCrc32Final(c.runCrc) != c.sectionCrc[c.section]) return Fail();
  SeekTile(static_cast<uint8_t>(c.section + 1), kTilePrefix, kClose);
  return StepResult::Continue;
}

HA_NOINLINE StepResult LayoutWriter::StepOpen() {
  LayoutWriteContext& c = *ctx_;
  if (!store_.OpenForReplace()) return Fail();
  c.fileOpen = true;
  c.stage = kHeader;
  return StepResult::Continue;
}

// The single directory commit.
HA_NOINLINE StepResult LayoutWriter::StepClose() {
  ctx_->fileOpen = false;  // Commit() closes the file whether or not it succeeds
  if (!store_.Commit()) return Fail();
  return Succeed();
}

// The first step's allocation, kept out of Step() so that Step() is a bare
// dispatch (its frame counts on every chain).
HA_NOINLINE bool LayoutWriter::StartWrite() {
  // One small block, freed by the last step.
  ctx_ = static_cast<LayoutWriteContext*>(pvPortMalloc(sizeof(LayoutWriteContext)));
  if (ctx_ == nullptr) return false;
  std::memset(static_cast<void*>(ctx_), 0, sizeof(*ctx_));
  ctx_->stage = kPrepLayout;
  state_.store(kRunning, std::memory_order_relaxed);
  return true;
}

StepResult LayoutWriter::Step() {
  const uint8_t st = state_.load(std::memory_order_acquire);
  if (st == kIdle) return StepResult::Idle;
  if (st == kPending && !StartWrite()) return Fail();

  switch (ctx_->stage) {
    case kPrepLayout:
      return StepPrepLayout();

    case kPrepDash:
      return StepPrepDash();

    case kPrepTile:
      return StepPrepTile();

    case kOpen:
      return StepOpen();

    case kHeader:
      return StepHeader();

    case kDashPiece:
      return StepDashPiece();

    case kTilePrefix:
      return StepTilePrefix();

    case kTileChunk:
      return StepTileChunk();

    default:
      return StepClose();
  }
}

}
}
}
