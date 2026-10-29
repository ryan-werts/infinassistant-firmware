#pragma once
// The flash-backed LayoutStore (REQ009-FR-B5): the persisted layout record as
// ONE littlefs file, replaced in place (3b.1 redo, "Shape C").
//
// A replacement is atomic without a rename. Opening the existing file with
// O_TRUNC only marks it dirty in RAM; the directory entry is replaced by the
// single commit that Commit() (littlefs's close) performs; a file that hit a
// write error is marked erred, and littlefs then closes it WITHOUT that commit.
// Abort() uses the same rule on purpose: it marks the handle erred before the
// close, so an aborted or failed replacement never commits and the previously
// committed record stands. On the very first push, a power loss between the
// create commit and the close leaves a zero-length entry, which the record
// parser reports as Absent.
//
// The write handle (`lfs_file_t`, 84 B) is a member: the write spans many timer
// callbacks, so it cannot live on a stack. Every method is a thin call into
// Controllers::FS (whose file calls are one-line wrappers over littlefs), so
// the stack this unit adds on the timer daemon is one small frame.
//
// Threading: one writer at a time, on the timer daemon. ReadAt() is for the
// display task and takes its own 84 B handle on its caller's stack; the heap
// phase in LayoutCommit.h keeps it from running while a replacement is open.
// No lock exists (littlefs is built without LFS_THREADSAFE): see the 3b.1
// record for the residual.
#include "LayoutStore.h"

#include <littlefs/lfs.h>

namespace Pinetime {
namespace Controllers {
class FS;
}
namespace Applications {
namespace HaBridge {

class LayoutStoreFlash final : public LayoutStore {
 public:
  static constexpr const char* kRecordPath = "/ha_layout.rec";

  /** The file system to write to and read from. Null until bound: every call then fails. */
  void Bind(Pinetime::Controllers::FS* fs) { fs_ = fs; }
  bool Bound() const { return fs_ != nullptr; }

  bool OpenForReplace() override;
  bool Write(const uint8_t* data, size_t len) override;
  bool Commit() override;
  void Abort() override;
  int ReadAt(size_t offset, uint8_t* out, size_t len) override;

 private:
  Pinetime::Controllers::FS* fs_ {nullptr};
  lfs_file_t file_;
  bool open_ {false};
};

}  // namespace HaBridge
}  // namespace Applications
}  // namespace Pinetime
