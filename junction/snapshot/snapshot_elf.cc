#include <cstdio>
#include <cstring>
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

namespace {

// Returns true if the page at mem matches the file content at file_offset.
// (Defined in snapshot.h as inline)

Status<std::tuple<std::vector<elf_phdr>, std::vector<iovec>,
                  std::vector<std::string>>>
GetElfPHDRs(MemoryMap &mm, SnapshotContext &ctx) {
  const std::vector<VMArea> vmas = mm.get_vmas();
  std::vector<elf_phdr> phdrs;
  std::vector<iovec> iovs;
  std::vector<std::string> path_strs;  // keeps path data alive for iovs
  size_t total_sections = vmas.size() + ctx.mem_areas_.size();
  phdrs.reserve(total_sections);
  iovs.reserve(total_sections);
  uint64_t offset = total_sections * sizeof(elf_phdr) + sizeof(elf_header);
  offset = PageAlign(offset);

  // Collect kPTypeFileRef entries separately so they are appended after all
  // kPTypeLoad PHDRs. This keeps offset page-aligned for load segments; the
  // unaligned path-string sizes only affect the trailing FileRef block.
  std::vector<elf_phdr> fileref_phdrs;
  std::vector<iovec> fileref_iovs;

  for (const VMArea &vma : vmas) {
    uint32_t flags = 0;
    if (vma.prot & PROT_EXEC) flags |= kFlagExec;
    if (vma.prot & PROT_WRITE) flags |= kFlagWrite;
    if (vma.prot & PROT_READ) flags |= kFlagRead;

    // Optimization: for file-backed VMAs whose pages match the backing file,
    // record the file path so the receiver can re-map instead of transferring.
    if (GetCfg().skip_file_pages() && vma.type == VMType::kFile &&
        !(vma.prot & PROT_WRITE)) {
      Status<std::string> path =
          vma.file->get_dent_ref().GetPathStr(FSRoot::GetGlobalRoot());
      if (path) {
        int fd = ksys_open(path->c_str(), O_RDONLY, 0);
        if (fd >= 0) {
          auto close_fd = finally([fd] { ksys_close(fd); });
          if (VMAMatchesFile(fd, vma)) {
            size_t pathsz = path->size() + 1;
            path_strs.push_back(std::move(*path));
            elf_phdr phdr = {
                .type = kPTypeFileRef,
                .flags = flags,
                .offset = 0,  // filled in after load segments
                .vaddr = vma.start,
                .paddr = static_cast<uint64_t>(vma.offset),
                .filesz = pathsz,
                .memsz = vma.Length(),
                .align = 1,
            };
            fileref_phdrs.push_back(phdr);
            fileref_iovs.emplace_back(
                const_cast<char *>(path_strs.back().c_str()), pathsz);
            continue;
          }
        }
      }
      // Fall through to Load if file can't be opened or pages are dirty.
    }

    size_t filesz = vma.DataLength();

    // Make memory area readable if needed.
    if (filesz && !(vma.prot & PROT_READ)) {
      auto ret = KernelMProtect(reinterpret_cast<void *>(vma.start), filesz,
                                vma.prot | PROT_READ);
      if (!ret) return MakeError(ret);
    }

    // Stacks grow downward: trim leading zero pages and only transfer the
    // live portion at the top, recording the offset into vaddr/memsz.
    if (vma.type == VMType::kStack && filesz) {
      size_t stack_off =
          GetStackMinOffset(reinterpret_cast<void *>(vma.start), filesz);
      uintptr_t live_start = vma.start + stack_off;
      size_t live_len = vma.Length() - stack_off;
      size_t live_filesz =
          PageAlign(GetMinSize(reinterpret_cast<void *>(live_start), live_len));
      elf_phdr phdr = {
          .type = kPTypeLoad,
          .flags = flags,
          .offset = offset,
          .vaddr = live_start,
          .paddr = 0,
          .filesz = live_filesz,
          .memsz = live_len,
          .align = kPageSize,
      };
      phdrs.push_back(phdr);
      if (live_filesz) {
        LOG(DEBUG) << "migration sender: Load PHDR vaddr=0x" << std::hex
                   << live_start << " type=" << vma.TypeString()
                   << " filesz=" << std::dec << live_filesz
                   << " memsz=" << live_len;
        offset += live_filesz;
        iovs.emplace_back(reinterpret_cast<void *>(live_start), live_filesz);
      }
      continue;
    }

    // Get rid of trailing zero pages.
    filesz = PageAlign(GetMinSize(reinterpret_cast<void *>(vma.start), filesz));

    elf_phdr phdr = {
        .type = kPTypeLoad,
        .flags = flags,
        .offset = offset,
        .vaddr = vma.start,
        .paddr = 0,             // don't care
        .filesz = filesz,       // size of data in the file
        .memsz = vma.Length(),  // total memory region size
        .align = kPageSize,     // align to page size
    };

    phdrs.push_back(phdr);

    if (filesz) {
      LOG(DEBUG) << "migration sender: Load PHDR vaddr=0x" << std::hex
                 << vma.start << " type=" << vma.TypeString()
                 << " filesz=" << std::dec << filesz
                 << " memsz=" << vma.Length();
      offset += filesz;
      iovs.emplace_back(reinterpret_cast<void *>(vma.start), filesz);
    }
  }

  for (const FSMemoryArea &area : ctx.mem_areas_) {
    size_t saved_area = PageAlign(GetMinSize(area.ptr, area.in_use_size));

    elf_phdr phdr = {
        .type = kPTypeLoad,
        .flags = kFlagRead | kFlagWrite,
        .offset = offset,
        .vaddr = reinterpret_cast<uintptr_t>(area.ptr),
        .paddr = 0,              // don't care
        .filesz = saved_area,    // size of data in the file
        .memsz = area.max_size,  // total memory region size
        .align = kPageSize,      // align to page size
    };
    phdrs.push_back(phdr);
    offset += saved_area;
    if (saved_area) iovs.emplace_back(area.ptr, saved_area);
  }

  // Now assign offsets to FileRef PHDRs and append them.
  for (size_t i = 0; i < fileref_phdrs.size(); i++) {
    fileref_phdrs[i].offset = offset;
    LOG(DEBUG) << "migration sender: FileRef PHDR path="
               << std::string_view(
                      static_cast<const char *>(fileref_iovs[i].iov_base),
                      fileref_phdrs[i].filesz - 1)
               << " vaddr=0x" << std::hex << fileref_phdrs[i].vaddr
               << " offset=0x" << fileref_phdrs[i].offset
               << " filesz=" << std::dec << fileref_phdrs[i].filesz
               << " memsz=" << fileref_phdrs[i].memsz;
    offset += fileref_phdrs[i].filesz;
    phdrs.push_back(fileref_phdrs[i]);
    iovs.push_back(fileref_iovs[i]);
  }

  return std::make_tuple(std::move(phdrs), std::move(iovs),
                         std::move(path_strs));
}

// Builds the ELF iovec list (header + phdrs + padding + data) and writes it
// to the given writer, then restores VMA protections.
Status<void> WriteElfIovecs(MemoryMap &mm, SnapshotContext &ctx,
                            VectoredWriter &out) {
  auto ret = GetElfPHDRs(mm, ctx);
  if (!ret) return MakeError(ret);
  auto &[pheaders, iovs, path_strs] = *ret;

  elf_header hdr;
  memset(&hdr, 0, sizeof(elf_header));
  hdr.magic[0] = '\177';
  hdr.magic[1] = 'E';
  hdr.magic[2] = 'L';
  hdr.magic[3] = 'F';
  hdr.magic[4] = kMagicClass64;
  hdr.magic[5] = kMagicData2LSB;
  hdr.magic[6] = kMagicVersion;
  hdr.type = kETypeExec;
  hdr.machine = kMachineAMD64;
  hdr.version = static_cast<uint32_t>(kMagicVersion);
  hdr.entry = 0;
  hdr.phoff = sizeof(elf_header);
  hdr.shoff = 0;
  hdr.flags = 0;
  hdr.ehsize = sizeof(elf_header);
  hdr.phsize = sizeof(elf_phdr);
  hdr.phnum = pheaders.size();
  hdr.shsize = 0;
  hdr.shnum = 0;
  hdr.shstrndx = 0;

  size_t header_size = sizeof(elf_header) + pheaders.size() * sizeof(elf_phdr);
  size_t padding = PageAlign(header_size) - header_size;
  std::array<std::byte, 4096> zeros{std::byte{0}};

  std::vector<iovec> elf_iovecs;
  elf_iovecs.reserve(pheaders.size() + iovs.size() + 2);
  elf_iovecs.emplace_back(&hdr, sizeof(elf_header));
  for (auto &pheader : pheaders)
    elf_iovecs.emplace_back(&pheader, sizeof(elf_phdr));
  if (padding > 0) elf_iovecs.emplace_back(zeros.data(), padding);
  elf_iovecs.insert(elf_iovecs.end(), iovs.begin(), iovs.end());

  if (Status<void> r = WritevFull(out, elf_iovecs); !r) return r;

  size_t elf_bytes = 0;
  for (const auto &iov : elf_iovecs) elf_bytes += iov.iov_len;
  LOG(INFO) << "migration: ELF bytes transferred: " << elf_bytes << " ("
            << (elf_bytes / 1024) << " KiB)";

  return RestoreVMAProtections(mm);
}

Status<void> SnapshotElf(MemoryMap &mm, SnapshotContext &ctx,
                         std::string_view elf_path) {
  Status<KernelFile> elf_file =
      KernelFile::Open(elf_path, O_CREAT | O_TRUNC, FileMode::kWrite, 0644);
  if (unlikely(!elf_file)) return MakeError(elf_file);
  return WriteElfIovecs(mm, ctx, *elf_file);
}

Status<void> SnapshotElfToStream(MemoryMap &mm, SnapshotContext &ctx,
                                 VectoredWriter &out) {
  return WriteElfIovecs(mm, ctx, out);
}

}  // namespace

Status<void> SnapshotProcToELF(Process *p, std::string_view metadata_path,
                               std::string_view elf_path) {
  LOG(INFO) << "snapshotting proc " << p->get_pid() << " into " << metadata_path
            << " and " << elf_path;

  StartSnapshotContext();

  Status<KernelFile> metadata_file = KernelFile::Open(
      metadata_path, O_CREAT | O_TRUNC, FileMode::kWrite, 0644);
  if (!metadata_file) return MakeError(metadata_file);

  auto f = finally([] { EndSnapshotContext(); });

  Status<void> ret = SnapshotMetadata(*p, *metadata_file);
  if (!ret) return ret;
  return SnapshotElf(p->get_mem_map(), GetSnapshotContext(), elf_path);
}

// Snapshots a process to a stream without touching the filesystem.
Status<void> SnapshotProcToELFStream(Process *p, VectoredWriter &out) {
  LOG(INFO) << "snapshotting proc " << p->get_pid() << " to stream";

  StartSnapshotContext();
  auto f = finally([] { EndSnapshotContext(); });

  Time t0 = Time::Now();
  Status<std::vector<std::byte>> metadata_buf = SerializeSnapshotMetadata(p);
  if (!metadata_buf) return MakeError(metadata_buf);
  Time t1 = Time::Now();
  LOG(INFO) << "migration sender: serialize took " << (t1 - t0).Microseconds()
            << " us (" << metadata_buf->size() << " bytes)";

  if (Status<void> ret = WriteStreamPrefix(out, MigrationType::kStopAndCopy,
                                           *metadata_buf);
      !ret)
    return ret;

  if (Status<void> ret =
          SnapshotElfToStream(p->get_mem_map(), GetSnapshotContext(), out);
      !ret)
    return ret;
  Time t2 = Time::Now();
  LOG(INFO) << "migration sender: transfer took " << (t2 - t1).Microseconds()
            << " us";
  LOG(INFO) << "migration sender: total took " << (t2 - t0).Microseconds()
            << " us";
  return {};
}

Status<void> SnapshotPidToELF(pid_t pid, std::string_view metadata_path,
                              std::string_view elf_path) {
  std::shared_ptr<Process> p = Process::Find(pid);
  if (!p) {
    LOG(WARN) << "couldn't find proc with pid " << pid;
    return MakeError(ESRCH);
  }

  LOG(INFO) << "stopping proc with pid " << pid;

  // TODO(snapshot): child procs, if any exist, should also be stopped + waited.
  p->JobControlStop();
  p->WaitForFullStop();
  auto f = finally([&] {
    if (GetCfg().snapshot_terminate())
      p->DoExit(0);
    else
      p->JobControlContinue();
  });
  return SnapshotProcToELF(p.get(), metadata_path, elf_path);
}

Status<std::shared_ptr<Process>> RestoreProcessFromELF(
    std::string_view metadata_path, std::string_view elf_path) {
  rt::RuntimeLibcGuard guard;

  Status<KernelFile> f = KernelFile::Open(metadata_path, 0, FileMode::kRead);
  if (unlikely(!f)) return MakeError(f);
  StreamBufferReader<KernelFile> w(*f);
  std::istream instream(&w);
  cereal::BinaryInputArchive ar(instream);

  if (Status<void> ret = FSRestore(ar); unlikely(!ret)) return MakeError(ret);
  timings().restore_metadata_start = Time::Now();

  std::shared_ptr<Process> p;
  ar(p);
  SerializeUnixSocketState(ar);
  timings().restore_data_start = Time::Now();

  Status<JunctionFile> elf =
      JunctionFile::Open(p->get_fs(), elf_path, 0, FileMode::kRead);
  if (unlikely(!elf)) return MakeError(elf);

  // Temporary hack: the elf loader will create entries in this fake map,
  // allowing the actual memory map to be restored by cereal.
  MemoryMap mm(nullptr, kMemoryMappingSize);
  mm.MarkAsFake();
  Status<elf_data> ret = LoadELF(mm, *elf, p->get_fs());
  if (GetCfg().restore_populate()) {
    mm.ForEachVMA([](const VMArea &vma) {
      if (!(vma.prot & PROT_READ)) return;
      KernelMAdvise(vma.Addr(), vma.Length(), MADV_POPULATE_READ);
    });
  }

  if (unlikely(!ret)) {
    LOG(ERR) << "Elf load failed: " << ret.error();
    return MakeError(ret);
  };

  if (unlikely(GetCfg().mem_trace())) p->get_mem_map().EnableTracing(*p.get());

  // mark threads as runnable
  // (must be last things to run, this will get the snapshot running)
  p->RunThreads();
  return p;
}

// Restores a process from a migration stream. Dispatches on MigrationType.
Status<std::shared_ptr<Process>> RestoreProcessFromELFStream(
    VectoredReader &in) {
  Time t0 = Time::Now();
  timings().migration_restore_start = t0;

  // Read stream prefix: type + metadata.
  auto prefix = ReadStreamPrefix(in);
  if (!prefix) return MakeError(prefix);
  auto &[type, metadata_buf] = *prefix;
  Time t1 = Time::Now();
  LOG(INFO) << "migration receiver: metadata transfer took "
            << (t1 - t0).Microseconds() << " us (" << metadata_buf.size()
            << " bytes)";

  // Deserialize metadata.
  auto p = DeserializeSnapshotMetadata(metadata_buf);
  if (!p) return MakeError(p);
  Time t2 = Time::Now();
  LOG(INFO) << "migration receiver: metadata deserialize took "
            << (t2 - t1).Microseconds() << " us";

  // Dispatch on migration type.
  if (type == MigrationType::kScatterCopy) {
    return RestoreFromScatterStream(in, std::move(*p));
  }

  if (type != MigrationType::kStopAndCopy) {
    LOG(ERR) << "unsupported migration type: " << static_cast<int>(type);
    return MakeError(EINVAL);
  }

  // ELF restore path: buffer to tmpfile, then LoadELF.
  Status<KernelFile> tmp =
      KernelFile::Open("/tmp/junction_migrate.elf", O_CREAT | O_TRUNC,
                       FileMode::kReadWrite, 0600);
  if (unlikely(!tmp)) return MakeError(tmp);

  size_t elf_bytes = 0;
  {
    std::array<std::byte, 65536> buf;
    while (true) {
      iovec iov = {buf.data(), buf.size()};
      Status<size_t> n = in.Readv({&iov, 1});
      if (!n || *n == 0) break;
      elf_bytes += *n;
      iovec wiov = {buf.data(), *n};
      if (Status<void> ret = WritevFull(*tmp, {&wiov, 1}); !ret)
        return MakeError(ret);
    }
  }
  Time t3 = Time::Now();
  LOG(INFO) << "migration receiver: ELF transfer took "
            << (t3 - t2).Microseconds() << " us (" << elf_bytes << " bytes)";

  Status<JunctionFile> elf = JunctionFile::Open(
      (*p)->get_fs(), "/tmp/junction_migrate.elf", 0, FileMode::kRead);
  if (unlikely(!elf)) return MakeError(elf);

  MemoryMap mm(nullptr, kMemoryMappingSize);
  mm.MarkAsFake();
  Status<elf_data> ret = LoadELF(mm, *elf, (*p)->get_fs());
  if (GetCfg().restore_populate()) {
    mm.ForEachVMA([](const VMArea &vma) {
      if (!(vma.prot & PROT_READ)) return;
      KernelMAdvise(vma.Addr(), vma.Length(), MADV_POPULATE_READ);
    });
  }

  if (unlikely(!ret)) {
    LOG(ERR) << "Elf load failed (stream restore): " << ret.error();
    return MakeError(ret);
  }
  Time t4 = Time::Now();
  LOG(INFO) << "migration receiver: ELF deserialize took "
            << (t4 - t3).Microseconds() << " us";
  LOG(INFO) << "migration receiver: total took " << (t4 - t0).Microseconds()
            << " us";

  if (unlikely(GetCfg().mem_trace()))
    (*p)->get_mem_map().EnableTracing(*(*p).get());

  timings().migration_restore_done = Time::Now();
  (*p)->RunThreads();
  return std::move(*p);
}

}  // namespace junction
