#include "FrameCodec.h"

#include <algorithm>
#include <cstring>

#include <FreeRTOS.h>
#include <portable.h>  // pvPortMalloc / vPortFree — the same FreeRTOS heap LVGL uses

namespace Pinetime {
namespace Applications {
namespace HaBridge {

static constexpr size_t kHeaderLen = 9;

// On-demand reassembly buffer (see FrameCodec.h for rationale). NULL when no
// multi-chunk transfer is in flight; pvPortMalloc'd for the exact totalLen at
// transfer start and freed immediately after the completed frame is decoded
// (ReleaseBuffer) or on abort/timeout. Previously a permanently-resident BSS array
// (kMaxReassemblySize bytes) — that held ~8 KB of the ~24 KB FreeRTOS heap hostage
// and starved HA-face rendering. Single-chunk frames never touch this buffer.
uint8_t* Reassembler::reassemblyBuf_ = nullptr;

void Reassembler::NoteTransferEnded(uint32_t nowMs) {
  endedMs_.store(nowMs, std::memory_order_relaxed);
  endedValid_.store(true, std::memory_order_release);
}

bool Reassembler::CooldownActive(uint32_t nowMs) const {
  if (cooldownMs_ == 0 || !endedValid_.load(std::memory_order_acquire)) {
    return false;
  }
  // Unsigned difference: correct across the 2^32 wrap of the caller's clock.
  return static_cast<uint32_t>(nowMs - endedMs_.load(std::memory_order_relaxed)) < cooldownMs_;
}

void Reassembler::EndGate() {
  if (gateHeld_) {
    gateHeld_ = false;
    // Start the cooldown before the phase goes back, so there is no instant at
    // which the phase is Idle and the cooldown is not yet running.
    NoteTransferEnded(lastNowMs_);
    if (gate_ != nullptr) {
      gate_->End();
    }
  }
}

// Every path that discards a transfer or its completed buffer comes through here,
// so the gate (the heap phase) is released with the buffer, on abort, timeout,
// supersede and an unconsumed frame alike.
void Reassembler::FreeBuffer() {
  if (reassemblyBuf_ != nullptr) {
    vPortFree(reassemblyBuf_);
    reassemblyBuf_ = nullptr;
  }
  completedFramePending_ = false;
  EndGate();
}

uint8_t* Reassembler::TakeBuffer(const uint8_t* payload) {
  if (!completedFramePending_ || reassemblyBuf_ == nullptr || payload != reassemblyBuf_) {
    return nullptr;
  }
  uint8_t* taken = reassemblyBuf_;
  reassemblyBuf_ = nullptr;
  completedFramePending_ = false;
  gateHeld_ = false;  // the taker owns the buffer and the transfer from here
  return taken;
}

void Reassembler::ReleaseBuffer() {
  // Only free once the consumer has decoded a completed multi-chunk frame.
  // A partial/in-flight transfer keeps its buffer.
  if (completedFramePending_) {
    FreeBuffer();
  }
}

static uint16_t ReadU16Le(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

static void WriteU16Le(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

std::optional<Frame> Reassembler::Ingest(const uint8_t* data, size_t len, uint32_t nowMs) {
  // Timeout check: if an in-flight message has been waiting too long, discard it
  // and free its on-demand buffer.
  ExpireStalled(nowMs);

  if (len < kHeaderLen) {
    return std::nullopt;
  }

  FrameHeader h{};
  h.version = data[0];
  h.type = data[1];
  h.flags = data[2];
  h.msgId = ReadU16Le(&data[3]);
  h.totalLen = ReadU16Le(&data[5]);
  h.chunkIndex = data[7];
  h.chunkCount = data[8];

  // Reject payloads that exceed the reassembly cap (the max the phone may send).
  if (h.totalLen > kMaxReassemblySize) {
    return std::nullopt;
  }

  if (h.chunkCount == 0 || h.chunkIndex >= h.chunkCount) {
    return std::nullopt;
  }

  const uint8_t* chunkData = data + kHeaderLen;
  size_t chunkLen = len - kHeaderLen;

  const bool gated = (gate_ != nullptr && h.type == gatedType_);

  // ── Single-chunk fast path: zero-copy borrowed pointer ──
  // 95%+ of messages (PING, STATE_UPDATE, CONFIG_ACK, etc.) fit in one frame.
  // A gated, non-empty message takes the buffered path below even in one chunk:
  // its consumer keeps the buffer past the callback.
  if (h.chunkCount == 1 && !(gated && h.totalLen > 0)) {
    if (chunkLen < h.totalLen) return std::nullopt;

    Frame frame;
    frame.header = h;
    frame.borrowedData = chunkData;  // Points into caller's stack buffer
    frame.borrowedLen = h.totalLen;
    return frame;
  }

  // The single-chunk path above takes exactly totalLen bytes of its chunk; keep
  // that for a gated single-chunk message.
  if (h.chunkCount == 1 && chunkLen > h.totalLen) {
    chunkLen = h.totalLen;
  }

  // ── Multi-chunk path: accumulate into the ON-DEMAND reassembly buffer ──
  // Only WATCHFACE_LAYOUT messages typically need this (2-8KB, 10-16 chunks). The
  // buffer is pvPortMalloc'd here for the exact totalLen and freed after the
  // completed frame is decoded (ReleaseBuffer) or on any abort/timeout below — so it
  // holds 0 resident bytes during rendering. BLE GATT writes arrive in order.

  // Begin a new transfer when: nothing in flight, a different msgId, or the meta of
  // the same-msgId transfer changed (totalLen/chunkCount/type). Both cases need a
  // (re)allocation sized to this transfer's totalLen.
  const bool startNew = (!inFlight_.active || inFlight_.header.msgId != h.msgId ||
                         inFlight_.header.totalLen != h.totalLen ||
                         inFlight_.header.chunkCount != h.chunkCount ||
                         inFlight_.header.type != h.type);
  if (startNew) {
    FreeBuffer();  // discard any superseded/aborted in-flight buffer (and its gate)
    inFlight_.active = false;

    // A transfer starts at chunk 0. Any later chunk with nothing matching in
    // flight belongs to a transfer that was refused, timed out or superseded:
    // drop it here, silently, before the gate or the allocator is touched.
    if (h.chunkIndex != 0) {
      if (gated) {
        gate_->OnChunkDropped();
      }
      return std::nullopt;
    }

    // First chunk of a gated transfer: take the heap phase before the guard and
    // before any allocation. Refusal is reported once per transfer.
    // A transfer that ended a moment ago (including the one this chunk just
    // superseded: startNew released it above) leaves the display its window.
    if (gated) {
      if (CooldownActive(nowMs)) {
        ++cooldownRefused_;
        gate_->OnRefused(h.msgId);
        return std::nullopt;
      }
      if (!gate_->TryBegin()) {
        gate_->OnRefused(h.msgId);
        return std::nullopt;
      }
      gateHeld_ = true;
    }

    // Refuse the transfer unless the heap can spare it WITH headroom.
    //
    // This buffer comes out of the same FreeRTOS heap LVGL renders from and
    // NimBLE draws its mbufs from, and it is held for the whole transfer plus
    // the decode. A fully-populated layout (4 screens / 4 tiles / 5 status
    // groups / 3 sensor groups — all at cap) encodes to ~4.0 KB, and measured
    // free heap with that dashboard already on screen is also ~4.0 KB. So the
    // unguarded malloc either fails outright or succeeds and leaves the system
    // at essentially zero, where the next mbuf or lv_*_create is what actually
    // falls over. Succeeding here and dying somewhere else is the worst of the
    // options; a clean refusal is recoverable and diagnosable.
    //
    // The phone's reassembly times out and it can retry once the heap frees up
    // (e.g. off the HA watch face), which is the same path as a malloc failure
    // below — just reached deterministically and BEFORE the heap is drained.
    const uint32_t freeHeap = static_cast<uint32_t>(xPortGetFreeHeapSize());
    if (freeHeap < static_cast<uint32_t>(h.totalLen) + kReassemblyHeadroomBytes) {
      ++refused_;
      EndGate();
      return std::nullopt;
    }

    reassemblyBuf_ = static_cast<uint8_t*>(pvPortMalloc(h.totalLen));
    if (reassemblyBuf_ == nullptr) {
      // Heap too tight at this instant — drop the message (no crash). The phone's
      // reassembly will time out and it can retry once the face frees heap.
      ++refused_;
      EndGate();
      return std::nullopt;
    }
    inFlight_.header = h;
    inFlight_.firstSeenMs = nowMs;
    inFlight_.bytesReceived = 0;
    inFlight_.chunksReceived = 0;
    inFlight_.active = true;
  }

  // Verify chunk arrives in expected order (BLE guarantees this, but be safe)
  if (h.chunkIndex != inFlight_.chunksReceived) {
    inFlight_.active = false;
    FreeBuffer();
    return std::nullopt;
  }

  // Bounds check against the allocated size (== totalLen).
  if (inFlight_.bytesReceived + chunkLen > inFlight_.header.totalLen) {
    inFlight_.active = false;
    FreeBuffer();
    return std::nullopt;
  }

  // Append chunk data to the on-demand buffer
  std::memcpy(reassemblyBuf_ + inFlight_.bytesReceived, chunkData, chunkLen);
  inFlight_.bytesReceived += static_cast<uint16_t>(chunkLen);
  inFlight_.chunksReceived++;

  // Not all chunks received yet — keep the buffer for the next chunk
  if (inFlight_.chunksReceived < h.chunkCount) {
    return std::nullopt;
  }

  // All chunks received — validate total length
  if (inFlight_.bytesReceived < h.totalLen) {
    inFlight_.active = false;
    FreeBuffer();
    return std::nullopt;
  }

  // Complete! Return a frame borrowing the on-demand buffer. The consumer decodes it
  // synchronously; FrameCodec::OnDataReceived then calls ReleaseBuffer() to free it.
  Frame frame;
  frame.header = inFlight_.header;
  frame.borrowedData = reassemblyBuf_;
  frame.borrowedLen = h.totalLen;

  inFlight_.active = false;
  completedFramePending_ = true;  // ReleaseBuffer() frees the buffer after decode
  return frame;
}

bool Reassembler::ExpireStalled(uint32_t nowMs) {
  lastNowMs_ = nowMs;
  if (inFlight_.active && (nowMs - inFlight_.firstSeenMs > timeoutMs_)) {
    inFlight_.active = false;
    FreeBuffer();
    return true;
  }
  return false;
}

void Reassembler::Clear(uint32_t nowMs) {
  lastNowMs_ = nowMs;
  inFlight_.active = false;
  FreeBuffer();
}

std::vector<std::vector<uint8_t>> Chunk(uint8_t version,
                                       uint8_t type,
                                       uint8_t flags,
                                       uint16_t msgId,
                                       const std::vector<uint8_t>& payload,
                                       uint16_t maxFrameBytes) {
  const size_t maxChunk = (maxFrameBytes > kHeaderLen) ? (maxFrameBytes - kHeaderLen) : 1;

  if (payload.size() > 0xFFFF) {
    return {};
  }

  size_t chunkCountSize = payload.empty() ? 1 : (payload.size() + maxChunk - 1) / maxChunk;

  if (chunkCountSize > 255) {
    return {};
  }
  const uint8_t chunkCount = static_cast<uint8_t>(chunkCountSize);

  std::vector<std::vector<uint8_t>> frames;
  frames.reserve(chunkCount);

  for (uint16_t i = 0; i < chunkCount; i++) {
    const size_t start = static_cast<size_t>(i) * maxChunk;
    const size_t end = std::min(payload.size(), start + maxChunk);

    std::vector<uint8_t> frame;
    frame.resize(kHeaderLen + (end - start));

    frame[0] = version;
    frame[1] = type;
    frame[2] = flags;
    WriteU16Le(&frame[3], msgId);
    WriteU16Le(&frame[5], static_cast<uint16_t>(payload.size()));
    frame[7] = static_cast<uint8_t>(i);
    frame[8] = static_cast<uint8_t>(chunkCount);

    if (end > start) {
      std::memcpy(&frame[kHeaderLen], payload.data() + start, end - start);
    }
    frames.push_back(std::move(frame));
  }

  return frames;
}

void FrameCodec::OnDataReceived(const uint8_t* data, size_t len, uint32_t nowMs,
                                std::function<void(const Frame&)> onFrameReady) {
  auto result = reassembler_.Ingest(data, len, nowMs);
  if (result.has_value() && onFrameReady) {
    onFrameReady(result.value());
  }
  // Free the on-demand multi-chunk buffer now that the consumer has decoded the
  // completed frame (no-op for single-chunk / partial transfers).
  reassembler_.ReleaseBuffer();
}

std::vector<std::vector<uint8_t>> FrameCodec::EncodeMessage(uint8_t type,
                                                            const std::vector<uint8_t>& payload) {
  uint16_t msgId = GetNextMsgId();
  return Chunk(0x01, type, 0x00, msgId, payload, mtuSize_);
}

} // namespace HaBridge
} // namespace Applications
} // namespace Pinetime
