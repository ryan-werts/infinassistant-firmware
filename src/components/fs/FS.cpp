#include "components/fs/FS.h"
#include <cstring>
#include <littlefs/lfs.h>
#include <lvgl/lvgl.h>
#include <FreeRTOS.h>
#include <task.h>

using namespace Pinetime::Controllers;

namespace {
  // Scoped lock over FS::busy_. littlefs is built without LFS_THREADSAFE and
  // several tasks reach the file system, so every call into it is serialized
  // here.
  //
  // A waiter sleeps for a tick rather than spinning: a littlefs call can block
  // for a sector erase, and the holder may be the lowest-priority task (the
  // display), which a spinning higher-priority waiter would starve.
  //
  // A FreeRTOS mutex is not used because its ~88 B heap block comes straight
  // out of the REQ-009 heap margin, which is only +62 B.
  //
  // The lock is not recursive: no FS method may call another FS method while
  // holding it.
  class FsLock {
  public:
    explicit FsLock(std::atomic<bool>& busy) : busy_ {busy} {
      while (busy_.exchange(true, std::memory_order_acquire)) {
        vTaskDelay(1);
      }
    }
    ~FsLock() {
      busy_.store(false, std::memory_order_release);
    }
    FsLock(const FsLock&) = delete;
    FsLock& operator=(const FsLock&) = delete;

  private:
    std::atomic<bool>& busy_;
  };
}

FS::FS(Pinetime::Drivers::SpiNorFlash& driver)
  : flashDriver {driver},
    lfsConfig {
      .context = this,
      .read = SectorRead,
      .prog = SectorProg,
      .erase = SectorErase,
      .sync = SectorSync,

      .read_size = 16,
      .prog_size = 8,
      .block_size = blockSize,
      .block_count = size / blockSize,
      .block_cycles = 1000u,

      .cache_size = 16,
      .lookahead_size = 16,

      .name_max = 50,
      .attr_max = 50,
    } {
}

void FS::Init() {
  FsLock lock {busy_};

  // try mount
  int err = lfs_mount(&lfs, &lfsConfig);

  // reformat if we can't mount the filesystem
  // this should only happen on the first boot
  if (err != LFS_ERR_OK) {
    lfs_format(&lfs, &lfsConfig);
    err = lfs_mount(&lfs, &lfsConfig);
    if (err != LFS_ERR_OK) {
      return;
    }
  }

#ifndef PINETIME_IS_RECOVERY
  VerifyResource();
#endif
}

void FS::VerifyResource() {
  // validate the resource metadata
  resourcesValid = true;
}

int FS::FileOpen(lfs_file_t* file_p, const char* fileName, const int flags) {
  FsLock lock {busy_};
  return lfs_file_open(&lfs, file_p, fileName, flags);
}

int FS::FileClose(lfs_file_t* file_p) {
  FsLock lock {busy_};
  return lfs_file_close(&lfs, file_p);
}

int FS::FileRead(lfs_file_t* file_p, uint8_t* buff, uint32_t size) {
  FsLock lock {busy_};
  return lfs_file_read(&lfs, file_p, buff, size);
}

int FS::FileWrite(lfs_file_t* file_p, const uint8_t* buff, uint32_t size) {
  FsLock lock {busy_};
  return lfs_file_write(&lfs, file_p, buff, size);
}

int FS::FileSeek(lfs_file_t* file_p, uint32_t pos) {
  FsLock lock {busy_};
  return lfs_file_seek(&lfs, file_p, pos, LFS_SEEK_SET);
}

int FS::FileDelete(const char* fileName) {
  FsLock lock {busy_};
  return lfs_remove(&lfs, fileName);
}

int FS::DirOpen(const char* path, lfs_dir_t* lfs_dir) {
  FsLock lock {busy_};
  return lfs_dir_open(&lfs, lfs_dir, path);
}

int FS::DirClose(lfs_dir_t* lfs_dir) {
  FsLock lock {busy_};
  return lfs_dir_close(&lfs, lfs_dir);
}

int FS::DirRead(lfs_dir_t* dir, lfs_info* info) {
  FsLock lock {busy_};
  return lfs_dir_read(&lfs, dir, info);
}

int FS::DirRewind(lfs_dir_t* dir) {
  FsLock lock {busy_};
  return lfs_dir_rewind(&lfs, dir);
}

int FS::DirCreate(const char* path) {
  FsLock lock {busy_};
  return lfs_mkdir(&lfs, path);
}

int FS::Rename(const char* oldPath, const char* newPath) {
  FsLock lock {busy_};
  return lfs_rename(&lfs, oldPath, newPath);
}

int FS::Stat(const char* path, lfs_info* info) {
  FsLock lock {busy_};
  return lfs_stat(&lfs, path, info);
}

lfs_ssize_t FS::GetFSSize() {
  FsLock lock {busy_};
  return lfs_fs_size(&lfs);
}

/*

    ----------- Interface between littlefs and SpiNorFlash -----------

*/
int FS::SectorSync(const struct lfs_config* /*c*/) {
  return 0;
}

int FS::SectorErase(const struct lfs_config* c, lfs_block_t block) {
  Pinetime::Controllers::FS& lfs = *(static_cast<Pinetime::Controllers::FS*>(c->context));
  const size_t address = startAddress + (block * blockSize);
  lfs.flashDriver.SectorErase(address);
  return lfs.flashDriver.EraseFailed() ? -1 : 0;
}

int FS::SectorProg(const struct lfs_config* c, lfs_block_t block, lfs_off_t off, const void* buffer, lfs_size_t size) {
  Pinetime::Controllers::FS& lfs = *(static_cast<Pinetime::Controllers::FS*>(c->context));
  const size_t address = startAddress + (block * blockSize) + off;
  lfs.flashDriver.Write(address, (uint8_t*) buffer, size);
  return lfs.flashDriver.ProgramFailed() ? -1 : 0;
}

int FS::SectorRead(const struct lfs_config* c, lfs_block_t block, lfs_off_t off, void* buffer, lfs_size_t size) {
  Pinetime::Controllers::FS& lfs = *(static_cast<Pinetime::Controllers::FS*>(c->context));
  const size_t address = startAddress + (block * blockSize) + off;
  lfs.flashDriver.Read(address, static_cast<uint8_t*>(buffer), size);
  return 0;
}
