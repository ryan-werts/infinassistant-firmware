#pragma once

#include "DashboardLimits.h"

#include <atomic>
#include <climits>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

// FrameHeader::totalLen is a uint16_t on the wire.  If the YAML ever sets
// MAX_REASSEMBLY_SIZE above UINT16_MAX the `h.totalLen > kMaxReassemblySize`
// guard in Reassembler::Ingest becomes vacuous: a uint16_t can never exceed
// 65535 but the right side would be 65536+.  Fail the build loudly instead.
static_assert(Pinetime::Applications::HaBridge::DashboardLimits::MAX_REASSEMBLY_SIZE
                  <= UINT16_MAX,
              "DashboardLimits::MAX_REASSEMBLY_SIZE must fit in FrameHeader::totalLen (uint16_t)");

namespace Pinetime {
namespace Applications {
namespace HaBridge {

struct FrameHeader {
  uint8_t version;
  uint8_t type;
  uint8_t flags;
  uint16_t msgId;
  uint16_t totalLen;
  uint8_t chunkIndex;
  uint8_t chunkCount;
};

// A complete reassembled frame.
// Uses a zero-copy borrowed pointer for every message; it is valid only until the
// next Ingest call (FrameCodec::OnDataReceived releases the buffer right after
// the handler returns).
// Single-chunk, ungated: points into the BLE callback's buffer.
// Multi-chunk, and every non-empty gated message (single-chunk included): points
// into the pvPortMalloc'd reassembly buffer, which a gated handler may take over
// with TakeBuffer().
struct Frame {
  FrameHeader header;

  const uint8_t* borrowedData{nullptr};
  size_t borrowedLen{0};

  const uint8_t* PayloadData() const { return borrowedData; }
  size_t PayloadLen() const { return borrowedLen; }
};

// The consumer of a gated transfer's lifecycle: the layout writer's heap phase
// (LayoutCommit.h), seen through the three calls the reassembler needs. The
// codec stays free of the writer so it links on its own.
//
//   TryBegin()   first chunk of a gated transfer, before anything is allocated.
//                False refuses the transfer (a push is pending, or a
//                materialization holds the phase).
//   End()        the transfer ended WITHOUT a hand-off: abort, timeout,
//                superseded, or a completed frame the consumer did not take.
//                Called at most once per successful TryBegin, and never after
//                TakeBuffer() moved the transfer to the consumer.
//   OnRefused()  TryBegin() said no; called once per refused transfer, with its
//                msgId. Later chunks of that msgId are dropped without another
//                call to OnRefused.
//   OnChunkDropped()
//                a chunk of a gated message that belongs to no transfer in
//                flight (its transfer was refused, timed out or superseded) was
//                dropped. Refused chunks never complete a frame, so this is the
//                consumer's chance to retry a refusal notice it could not send.
//                Optional: the default does nothing.
class TransferGate {
 public:
  virtual bool TryBegin() = 0;
  virtual void End() = 0;
  virtual void OnRefused(uint16_t msgId) = 0;
  virtual void OnChunkDropped() {}

 protected:
  ~TransferGate() = default;
};

class Reassembler {
 public:
  // Default timeout sourced from DashboardLimits (generated from
  // firmware-limits.yaml) — the value reported to the phone via HELLO and the
  // value the reassembler actually enforces cannot drift.  See BRD Resolved
  // Decision #8.
  explicit Reassembler(uint32_t timeoutMs = DashboardLimits::REASSEMBLY_TIMEOUT_MS,
                       uint32_t cooldownMs = kTransferCooldownMs)
      : timeoutMs_(timeoutMs), cooldownMs_(cooldownMs) {}

  // Fairness between the phone and the display. After a gated transfer ENDS
  // (hand-off write finished, abort, timeout, stall, disconnect, supersede) a NEW
  // gated transfer is refused, with the same refusal as a held heap phase, for
  // this long. It is longer than one display refresh (1000 ms), so the heap
  // phase is Idle for at least one refresh tick between any two transfers and a
  // peer sending back-to-back or superseding chunk-0 transfers cannot starve the
  // display's materialization. The clock is the caller's `nowMs`; every
  // comparison is an unsigned difference, so it holds across the 2^32 wrap.
  static constexpr uint32_t kTransferCooldownMs = 1100;
  static_assert(kTransferCooldownMs >= 1100, "the cooldown must outlast one 1000 ms display refresh");

  // Feed a single BLE frame. Returns a complete frame when all chunks arrive.
  // ZERO heap allocation — single-chunk uses caller's buffer, multi-chunk uses
  // a static BSS buffer. No std::vector, no std::unordered_map.
  std::optional<Frame> Ingest(const uint8_t* data, size_t len, uint32_t nowMs);

  // Abort the in-flight transfer if it began more than the reassembly timeout
  // before `nowMs`: the buffer is freed and the gate ended, exactly as a late
  // frame would do at the top of Ingest(). Returns true when it aborted one.
  // Ingest() calls it first; the layout pipeline also calls it from a NimBLE
  // callout, so a transfer the phone abandoned does not hold the heap phase
  // until the next frame. Same task as Ingest(): no locking.
  bool ExpireStalled(uint32_t nowMs);

  // Drop the in-flight transfer (the link went away) at time `nowMs`.
  void Clear(uint32_t nowMs);

  // A gated transfer that the codec no longer tracks has ended: the writer
  // finished (or gave up on) the buffer it took with TakeBuffer(). Starts the
  // cooldown. Safe from another task than Ingest (two atomic stores).
  void NoteTransferEnded(uint32_t nowMs);

  // Free the on-demand multi-chunk buffer after a completed frame has been
  // consumed. Called by FrameCodec::OnDataReceived immediately after onFrameReady().
  // No-op unless the last Ingest() returned a completed multi-chunk frame (the
  // buffer must stay alive for partial/in-flight transfers).
  void ReleaseBuffer();

  // Gate the transfers of message type `type` (WATCHFACE_LAYOUT). A gated
  // message always uses the buffered path, single-chunk included, so the
  // consumer can take ownership of its buffer; every other type is unchanged.
  void SetTransferGate(TransferGate* gate, uint8_t type) {
    gate_ = gate;
    gatedType_ = type;
  }

  // Ownership transfer. `payload` must be the pointer of the completed gated
  // frame Ingest() just returned. On success the caller owns the pvPortMalloc
  // buffer and the transfer: it will be freed with vPortFree by the caller (or
  // its successor), and the gate's End() is NOT called for it. Returns nullptr,
  // changing nothing, for any other pointer or state. The buffer is released
  // exactly once: here by the reassembler until this call succeeds, by the taker
  // after.
  uint8_t* TakeBuffer(const uint8_t* payload);

  // OBS-1: transfers dropped at the heap guard or at a failed allocation.
  uint32_t refused() const { return refused_; }
  // Transfers refused because they began inside the cooldown.
  uint32_t cooldownRefused() const { return cooldownRefused_; }

 private:
  // On-demand reassembly buffer for multi-chunk messages (WATCHFACE_LAYOUT).
  // pvPortMalloc'd (the SAME FreeRTOS heap LVGL draws from, LV_MEM_CUSTOM=1) for the
  // exact totalLen, only while a transfer is in flight, and freed immediately after
  // the completed frame is decoded (ReleaseBuffer) or on abort/timeout. So it holds
  // 0 resident bytes during rendering — handing the HA face ~8 KB more heap — while
  // still accepting layouts up to kMaxReassemblySize. Cap sourced from DashboardLimits
  // so HELLO's maxReassemblySize and the accepted size cannot drift. (Earlier this was
  // a permanently-resident BSS array; that starved the render heap.)
  static constexpr size_t kMaxReassemblySize = DashboardLimits::MAX_REASSEMBLY_SIZE;

  // Heap that must remain FREE after the reassembly buffer is taken. The buffer
  // is held for the whole multi-chunk transfer plus the decode, and it shares a
  // heap with NimBLE's mbufs and every lv_*_create, so taking the last of it
  // moves the failure somewhere far less recoverable.
  //
  // Sized above kFreeHeapSafeThresholdBytes (3072, the safe-face floor in
  // screens/HomeAssistant.h): staying over that floor is what keeps the watch
  // rendering the real face instead of "layout too heavy", so a push must not
  // be the thing that drags it under.
  static constexpr uint32_t kReassemblyHeadroomBytes = 3584;

  static uint8_t* reassemblyBuf_;

  void FreeBuffer();  // vPortFree reassemblyBuf_ if allocated; clears the completion flag; ends the gate
  void EndGate();     // gate_->End() once, if this reassembler began a transfer it still holds

  struct InFlight {
    FrameHeader header{};
    uint32_t firstSeenMs{0};
    uint16_t bytesReceived{0};
    uint8_t chunksReceived{0};
    bool active{false};
  };

  bool CooldownActive(uint32_t nowMs) const;

  uint32_t timeoutMs_;
  uint32_t cooldownMs_;
  InFlight inFlight_;
  bool completedFramePending_{false};  // buffer holds a returned-but-not-yet-freed completed frame
  TransferGate* gate_{nullptr};
  uint8_t gatedType_{0};
  bool gateHeld_{false};   // TryBegin() succeeded and neither End() nor TakeBuffer() has run since
  uint32_t refused_{0};
  uint32_t cooldownRefused_{0};
  // The cooldown's start. Written by Ingest's task and by the layout timer task
  // (NoteTransferEnded), read by Ingest: a value and a validity flag, both atomic
  // bytes/words that are lock-free on the target.
  std::atomic<uint32_t> endedMs_{0};
  std::atomic<bool> endedValid_{false};
  uint32_t lastNowMs_{0};  // the newest `nowMs` an entry point was given; EndGate stamps the cooldown with it
};

// Encode a payload into chunked frames, given maximum frame size (ATT payload). Header is 9 bytes.
std::vector<std::vector<uint8_t>> Chunk(uint8_t version,
                                       uint8_t type,
                                       uint8_t flags,
                                       uint16_t msgId,
                                       const std::vector<uint8_t>& payload,
                                       uint16_t maxFrameBytes);

/**
 * FrameCodec wraps the Reassembler for incoming frames and provides
 * chunking for outgoing frames.
 */
class FrameCodec {
 public:
  explicit FrameCodec(uint16_t mtuSize = 247, uint32_t cooldownMs = Reassembler::kTransferCooldownMs)
      : reassembler_(DashboardLimits::REASSEMBLY_TIMEOUT_MS, cooldownMs), mtuSize_(mtuSize) {}

  // Process incoming BLE data (raw pointer — no vector allocation).
  void OnDataReceived(const uint8_t* data, size_t len, uint32_t nowMs,
                     std::function<void(const Frame&)> onFrameReady);

  // Encode a message into BLE frames ready to send
  std::vector<std::vector<uint8_t>> EncodeMessage(uint8_t type,
                                                  const std::vector<uint8_t>& payload);

  void SetMtu(uint16_t mtu) { mtuSize_ = mtu; }
  // REQ-016 — atomic fetch_add. Both BLE and UI send paths share the codec
  // (SendFromBle / SendFromUi), so a non-atomic post-increment can produce
  // duplicate msgIds under concurrent issuance.
  uint16_t GetNextMsgId() { return nextMsgId_.fetch_add(1, std::memory_order_relaxed); }
  void Clear(uint32_t nowMs) { reassembler_.Clear(nowMs); }
  void NoteTransferEnded(uint32_t nowMs) { reassembler_.NoteTransferEnded(nowMs); }
  bool ExpireStalled(uint32_t nowMs) { return reassembler_.ExpireStalled(nowMs); }

  // See Reassembler. The handler of a gated frame takes the reassembly buffer
  // from inside the onFrameReady callback; OnDataReceived releases whatever the
  // handler did not take.
  void SetTransferGate(TransferGate* gate, uint8_t type) { reassembler_.SetTransferGate(gate, type); }
  uint8_t* TakeBuffer(const uint8_t* payload) { return reassembler_.TakeBuffer(payload); }
  uint32_t refused() const { return reassembler_.refused(); }
  uint32_t cooldownRefused() const { return reassembler_.cooldownRefused(); }

 private:
  Reassembler reassembler_;
  uint16_t mtuSize_;
  std::atomic<uint16_t> nextMsgId_{0};
};

} // namespace HaBridge
} // namespace Applications
} // namespace Pinetime
