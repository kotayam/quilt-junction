#include <cstdio>
#include <fstream>
#include <iostream>
#include <utility>

extern "C" {
#include <fcntl.h>
#include <signal.h>
}

#include "junction/base/error.h"
#include "junction/base/finally.h"
#include "junction/fs/file.h"
#include "junction/fs/fs.h"
#include "junction/fs/junction_file.h"
#include "junction/fs/memfs/memfs.h"
#include "junction/kernel/elf.h"
#include "junction/kernel/ksys.h"
#include "junction/kernel/proc.h"
#include "junction/kernel/usys.h"
#include "junction/net/unix.h"
#include "junction/snapshot/cereal.h"
#include "junction/snapshot/snapshot.h"

namespace junction {

static std::unique_ptr<SnapshotContext> cur_context;
static StartupTimings startup_timings;

StartupTimings &timings() { return startup_timings; }

SnapshotContext &GetSnapshotContext() {
  if (unlikely(!cur_context)) throw std::runtime_error("not doing a snapshot");
  return *cur_context.get();
}

void StartSnapshotContext() {
  assert(!cur_context);
  if (!GetCfg().expecting_snapshot()) {
    LOG(WARN) << "WARNING: starting snapshot on runtime was not expecting it; "
                 "consider re-running with the --snapshot_enabled flag";
  }
  cur_context.reset(new SnapshotContext);
}

void EndSnapshotContext() {
  assert(cur_context);
  cur_context.reset();
}
Status<void> SnapshotMetadata(Process &p, KernelFile &file) {
  rt::RuntimeLibcGuard guard;

  StreamBufferWriter<KernelFile> w(file);
  std::ostream outstream(&w);
  cereal::BinaryOutputArchive ar(outstream);
  if (Status<void> ret = FSSnapshot(ar); !ret) return ret;
  ar(p.shared_from_this());
  SerializeUnixSocketState(ar);
  return {};
}

Status<void> RestoreVMAProtections(MemoryMap &mm) {
  const std::vector<VMArea> vmas = mm.get_vmas();
  for (const VMArea &vma : vmas) {
    if (vma.prot & PROT_READ) continue;

    size_t filesz = vma.DataLength();
    if (!filesz) continue;

    Status<void> ret =
        KernelMProtect(reinterpret_cast<void *>(vma.start), filesz, vma.prot);

    if (!ret) return MakeError(ret);
  }
  return {};
}

Status<void> TakeSnapshot(Process *p) {
  const std::string prefix = GetCfg().GetArg("snapshot-prefix");
  if (GetCfg().jif())
    return SnapshotProcToJIF(p, prefix + ".jm", prefix + ".jif");
  return SnapshotProcToELF(p, prefix + ".metadata", prefix + ".elf");
}

Status<std::vector<std::byte>> SerializeSnapshotMetadata(Process *p) {
  std::vector<std::byte> buf;
  {
    rt::RuntimeLibcGuard guard;
    struct VecWriter {
      std::vector<std::byte> &buf;
      Status<size_t> Write(std::span<const std::byte> src) {
        buf.insert(buf.end(), src.begin(), src.end());
        return src.size();
      }
    } vw{buf};
    StreamBufferWriter<VecWriter> sbw(vw);
    std::ostream outstream(&sbw);
    cereal::BinaryOutputArchive ar(outstream);
    if (Status<void> ret = FSSnapshot(ar); !ret) return MakeError(ret);
    ar(p->shared_from_this());
    SerializeUnixSocketState(ar);
  }
  return buf;
}

Status<std::shared_ptr<Process>> DeserializeSnapshotMetadata(
    std::span<const std::byte> buf) {
  std::shared_ptr<Process> p;
  {
    rt::RuntimeLibcGuard guard;
    struct VecReader {
      std::span<const std::byte> remaining;
      Status<size_t> Read(std::span<std::byte> dst) {
        size_t n = std::min(dst.size(), remaining.size());
        std::copy_n(remaining.begin(), n, dst.begin());
        remaining = remaining.subspan(n);
        return n ? n : Status<size_t>(MakeError(EUNEXPECTEDEOF));
      }
    } vr{buf};
    StreamBufferReader<VecReader> sbr(vr);
    std::istream instream(&sbr);
    cereal::BinaryInputArchive ar(instream);

    if (Status<void> ret = FSRestore(ar); unlikely(!ret)) return MakeError(ret);
    timings().restore_metadata_start = Time::Now();

    ar(p);
    SerializeUnixSocketState(ar);
    timings().restore_data_start = Time::Now();
  }
  return p;
}

Status<void> WriteStreamPrefix(VectoredWriter &out, MigrationType type,
                               std::span<const std::byte> metadata) {
  if (Status<void> ret = WriteU8(out, static_cast<uint8_t>(type)); !ret)
    return ret;
  uint64_t len = metadata.size();
  if (Status<void> ret = WriteU64LE(out, len); !ret) return ret;
  iovec iov = {const_cast<std::byte *>(metadata.data()), metadata.size()};
  return WritevFull(out, {&iov, 1});
}

Status<std::pair<MigrationType, std::vector<std::byte>>> ReadStreamPrefix(
    VectoredReader &in) {
  // Read migration type byte.
  uint8_t type_byte = 0;
  {
    iovec iov = {&type_byte, sizeof(type_byte)};
    if (Status<void> ret = ReadvFull(in, {&iov, 1}); !ret)
      return MakeError(ret);
  }

  // Read metadata length prefix.
  uint64_t metadata_len = 0;
  {
    iovec iov = {&metadata_len, sizeof(metadata_len)};
    if (Status<void> ret = ReadvFull(in, {&iov, 1}); !ret)
      return MakeError(ret);
  }

  // Read metadata into buffer.
  std::vector<std::byte> buf(metadata_len);
  {
    iovec iov = {buf.data(), buf.size()};
    if (Status<void> ret = ReadvFull(in, {&iov, 1}); !ret)
      return MakeError(ret);
  }

  return std::make_pair(static_cast<MigrationType>(type_byte), std::move(buf));
}

}  // namespace junction
