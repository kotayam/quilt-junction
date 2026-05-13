// snapshot.h - tools for creating snapshots

#pragma once

extern "C" {
#include <signal.h>
#include <sys/resource.h>
#include <sys/uio.h>

#include "lib/caladan/runtime/defs.h"
}

#include <array>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "junction/base/arch.h"
#include "junction/base/bits.h"
#include "junction/base/error.h"
#include "junction/base/io.h"
#include "junction/base/time.h"
#include "junction/bindings/net.h"
#include "junction/kernel/elf.h"
#include "junction/kernel/ksys.h"
#include "junction/kernel/proc.h"
#include "junction/kernel/sigframe.h"
#include "junction/kernel/signal.h"

namespace junction {

namespace {
inline size_t GetMinSize(std::span<const uint64_t> buf) {
  auto it = std::find_if(buf.rbegin(), buf.rend(),
                         [](const uint64_t &c) { return c != 0; });
  return std::distance(buf.begin(), it.base());
}

inline size_t GetMinSize(const void *buf, size_t len) {
  len = div_up(len, sizeof(uint64_t));
  return GetMinSize({reinterpret_cast<const uint64_t *>(buf), len}) *
         sizeof(uint64_t);
}

// For stacks (which grow downward), returns the offset of the first non-zero
// page from the base, page-aligned down.
inline size_t GetStackMinOffset(const void *buf, size_t len) {
  const auto *words = reinterpret_cast<const uint64_t *>(buf);
  size_t nwords = div_up(len, sizeof(uint64_t));
  auto it = std::find_if(words, words + nwords,
                         [](const uint64_t &c) { return c != 0; });
  size_t byte_off = std::distance(words, it) * sizeof(uint64_t);
  return PageAlignDown(byte_off);
}
}  // anonymous namespace

struct StartupTimings {
  std::optional<Time> junction_main_start;
  std::optional<Time> restore_start;
  std::optional<Time> exec_start;
  std::optional<Time> restore_metadata_start;
  std::optional<Time> restore_data_start;
  std::optional<Time> first_function_start;
  std::optional<Time> first_function_end;
  std::optional<Time> migration_restore_start;
  std::optional<Time> migration_restore_done;

  Duration CaladanStartTime() {
    assert(junction_main_start);
    return *junction_main_start - Time(0);
  }

  Duration JunctionInitTime() {
    assert(junction_main_start);
    if (exec_start) {
      return *exec_start - *junction_main_start;
    } else {
      assert(restore_start);
      return *restore_start - *junction_main_start;
    }
  }

  Duration ApplicationInitTime() {
    assert(first_function_start && exec_start);
    return *first_function_start - *exec_start;
  }

  Duration FSRestoreTime() {
    assert(restore_metadata_start && restore_start);
    return *restore_metadata_start - *restore_start;
  }

  Duration MetadataRestoreTime() {
    assert(restore_data_start && restore_metadata_start);
    return *restore_data_start - *restore_metadata_start;
  }

  Duration DataRestoreTime() {
    assert(first_function_start && restore_data_start);
    return *first_function_start - *restore_data_start;
  }

  Duration FirstIterTime() {
    assert(first_function_end && first_function_start);
    return *first_function_end - *first_function_start;
  }

  Duration TotalRestoreTime() {
    assert(first_function_start && restore_start);
    return *first_function_start - *restore_start;
  }
};

StartupTimings &timings();

/**
 * Snapshot Context
 */
struct FSMemoryArea {
  char *ptr;
  size_t in_use_size;
  size_t max_size;
};

struct SnapshotContext {
  std::vector<FSMemoryArea> mem_areas_;
  std::vector<std::shared_ptr<DirectoryEntry>> dents;
};

SnapshotContext &GetSnapshotContext();
void StartSnapshotContext();
void EndSnapshotContext();

/**
 * General utilities
 */
Status<void> SnapshotMetadata(Process &p, KernelFile &file);
Status<void> RestoreVMAProtections(MemoryMap &mm);
Status<void> TakeSnapshot(Process *p);

/**
 * Migration
 */
enum class MigrationType : uint8_t {
  kStopAndCopy = 1,
  kScatterCopy = 2,
};

/**
 * Shared migration stream helpers
 */

// Write a uint64 in little-endian to a vectored writer.
inline Status<void> WriteU64LE(VectoredWriter &w, uint64_t v) {
  iovec iov = {&v, sizeof(v)};
  return WritevFull(w, {&iov, 1});
}

// Write a single byte to a vectored writer.
inline Status<void> WriteU8(VectoredWriter &w, uint8_t v) {
  iovec iov = {&v, sizeof(v)};
  return WritevFull(w, {&iov, 1});
}

// Returns true if the page at mem matches the file content at file_offset.
inline bool PageMatchesFile(int fd, off_t file_offset, const void *mem) {
  alignas(64) std::array<std::byte, kPageSize> buf;
  ssize_t n = ksys_pread(fd, buf.data(), kPageSize, file_offset);
  if (n != static_cast<ssize_t>(kPageSize)) return false;
  return std::memcmp(mem, buf.data(), kPageSize) == 0;
}

// Returns true if every page of the VMA matches the backing file content.
inline bool VMAMatchesFile(int fd, const VMArea &vma) {
  for (uintptr_t page = vma.start; page < vma.start + vma.Length();
       page += kPageSize) {
    off_t file_off = vma.offset + (page - vma.start);
    if (!PageMatchesFile(fd, file_off, reinterpret_cast<void *>(page)))
      return false;
  }
  return true;
}

// Serialize process metadata (FS + Process + Unix sockets) into a byte buffer.
Status<std::vector<std::byte>> SerializeSnapshotMetadata(Process *p);

// Deserialize process metadata from a byte buffer. Returns the Process.
Status<std::shared_ptr<Process>> DeserializeSnapshotMetadata(
    std::span<const std::byte> buf);

// Write the stream prefix: [1-byte type][8-byte metadata_len][metadata].
Status<void> WriteStreamPrefix(VectoredWriter &out, MigrationType type,
                               std::span<const std::byte> metadata);

// Read the stream prefix: type byte + length-prefixed metadata buffer.
Status<std::pair<MigrationType, std::vector<std::byte>>> ReadStreamPrefix(
    VectoredReader &in);

/**
 * ELF utilities
 */
Status<void> SnapshotPidToELF(pid_t pid, std::string_view metadata_path,
                              std::string_view elf_path);

Status<void> SnapshotProcToELF(Process *p, std::string_view metadata_path,
                               std::string_view elf_path);

// Snapshots a process directly to a stream (diskless).
// Stream format (kStopAndCopy): [1-byte MigrationType][8-byte metadata length
// LE][metadata bytes][ELF bytes]
Status<void> SnapshotProcToELFStream(Process *p, VectoredWriter &out);

Status<std::shared_ptr<Process>> RestoreProcessFromELF(
    std::string_view metadata_path, std::string_view elf_path);

// Restores a process from a stream (dispatches on MigrationType).
Status<std::shared_ptr<Process>> RestoreProcessFromELFStream(
    VectoredReader &in);

/**
 * Scatter-copy migration (skip ELF format entirely)
 * Stream format (kScatterCopy): [1-byte MigrationType][8-byte metadata length
 * LE][metadata bytes][MigrateHeader][MigrateSegment × N][kLoad page
 * data][kFileRef path strings]
 */
Status<void> SnapshotProcToScatterStream(Process *p, VectoredWriter &out);
Status<std::shared_ptr<Process>> RestoreFromScatterStream(
    VectoredReader &in, std::shared_ptr<Process> p);

/**
 * JIF utilities
 */
Status<void> SnapshotPidToJIF(pid_t pid, std::string_view metadata_path,
                              std::string_view jif_path);

Status<void> SnapshotProcToJIF(Process *p, std::string_view metadata_path,
                               std::string_view jif_path);

Status<std::shared_ptr<Process>> RestoreProcessFromJIF(
    std::string_view metadata_path, std::string_view jif_path);

}  // namespace junction
