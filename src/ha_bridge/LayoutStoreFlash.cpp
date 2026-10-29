#include "LayoutStoreFlash.h"

#include "components/fs/FS.h"

namespace Pinetime {
namespace Applications {
namespace HaBridge {

bool LayoutStoreFlash::OpenForReplace() {
  if (fs_ == nullptr || open_) {
    return false;
  }
  if (fs_->FileOpen(&file_, kRecordPath, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) < 0) {
    return false;  // littlefs closed the handle it failed to open: nothing is open
  }
  open_ = true;
  return true;
}

bool LayoutStoreFlash::Write(const uint8_t* data, size_t len) {
  if (!open_) {
    return false;
  }
  return fs_->FileWrite(&file_, data, static_cast<uint32_t>(len)) == static_cast<int>(len);
}

bool LayoutStoreFlash::Commit() {
  if (!open_) {
    return false;
  }
  open_ = false;
  return fs_->FileClose(&file_) >= 0;
}

void LayoutStoreFlash::Abort() {
  if (!open_) {
    return;
  }
  open_ = false;
  // An erred handle is closed without the directory commit.
  file_.flags |= LFS_F_ERRED;
  fs_->FileClose(&file_);
}

int LayoutStoreFlash::ReadAt(size_t offset, uint8_t* out, size_t len) {
  if (fs_ == nullptr) {
    return -1;
  }
  lfs_file_t file;
  const int opened = fs_->FileOpen(&file, kRecordPath, LFS_O_RDONLY);
  if (opened == LFS_ERR_NOENT) {
    return 0;  // no record
  }
  if (opened < 0) {
    return opened;
  }

  int result = 0;
  if (offset != 0 && fs_->FileSeek(&file, static_cast<uint32_t>(offset)) < 0) {
    result = -1;
  }
  size_t got = 0;
  while (result == 0 && got < len) {
    const int n = fs_->FileRead(&file, out + got, static_cast<uint32_t>(len - got));
    if (n < 0) {
      result = n;
    } else if (n == 0) {
      break;  // end of file
    } else {
      got += static_cast<size_t>(n);
    }
  }
  fs_->FileClose(&file);
  return result < 0 ? result : static_cast<int>(got);
}

}  // namespace HaBridge
}  // namespace Applications
}  // namespace Pinetime
