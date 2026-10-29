#pragma once

// The narrow store seam under the layout commit logic (REQ009-FR-C8b,
// REQ009-FR-B5).
//
// LayoutCommit.{h,cpp} reaches the persisted layout record only through this
// interface, so it links on the host with no file system, no littlefs and no
// SPI code. The firmware implements it over the file-system controller (one
// record file, replaced in place); the host tests implement it in memory
// (tools/fuzz/hoststubs/FakeLayoutStore.h) and can make it fail, read short or
// return a corrupt record.
//
// The record is ONE file. A replacement is atomic: nothing of it is visible
// until Commit(), and a replacement that is aborted, or that hit a write error,
// leaves the previously committed record exactly as it was.
//
// Call protocol (one writer at a time; the heap phase in LayoutCommit.h keeps a
// reader and the writer apart):
//
//   OpenForReplace()   Begin a replacement. Truncating: the new record is
//                      written from offset 0. Returns false on failure, and
//                      then nothing is open (no Abort() is needed).
//   Write(data, len)   Append to the open replacement. Returns false on
//                      failure; the replacement is then erred and the caller
//                      MUST call Abort(). Never called with nothing open.
//   Commit()           Close the replacement: the single directory commit.
//                      Returns true only when the new record is durable. After
//                      it returns, nothing is open either way, and on false the
//                      previous record stands.
//   Abort()            Close the replacement WITHOUT committing it. A no-op
//                      when nothing is open.
//   ReadAt(off,out,n)  Read the COMMITTED record. Returns the number of bytes
//                      read (0 <= r <= n; fewer than n at end of file, 0 when
//                      there is no record) or a negative value on an I/O error.
//                      Independent of any open replacement.
//
// No method may block on the caller's behalf beyond the store's own bus and
// flash waits, and none may be called from the GATT callback: the writer runs
// on the deferred step context (LayoutWriter::Step).

#include <cstddef>
#include <cstdint>

namespace Pinetime {
namespace Applications {
namespace HaBridge {

class LayoutStore {
 public:
  virtual bool OpenForReplace() = 0;
  virtual bool Write(const uint8_t* data, size_t len) = 0;
  virtual bool Commit() = 0;
  virtual void Abort() = 0;
  virtual int ReadAt(size_t offset, uint8_t* out, size_t len) = 0;

 protected:
  ~LayoutStore() = default;
};

}
}
}
