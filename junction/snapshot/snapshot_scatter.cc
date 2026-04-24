#include <cstring>
#include <vector>

extern "C" {
#include <fcntl.h>
}

#include "junction/base/error.h"
#include "junction/base/finally.h"
#include "junction/base/io.h"
#include "junction/bindings/log.h"
#include "junction/fs/fs.h"
#include "junction/fs/junction_file.h"
#include "junction/junction.h"
#include "junction/kernel/ksys.h"
#include "junction/kernel/proc.h"
#include "junction/limits.h"
#include "junction/snapshot/migrate_format.h"
#include "junction/snapshot/snapshot.h"

namespace junction {

namespace {

// Builds MigrateSegment descriptors and parallel iovecs from process VMAs.
// kLoad segments first, kFileRef segments appended after.
Status<std::tuple<std::vector<MigrateSegment>, std::vector<iovec>,
                  std::vector<std::string>>>
BuildMigrateSegments(MemoryMap &mm, SnapshotContext &ctx) {
  const std::vector<VMArea> vmas = mm.get_vmas();
  std::vector<MigrateSegment> segs;
  std::vector<iovec> iovs;
  std::vector<std::string> path_strs;

  std::vector<MigrateSegment> fileref_segs;
  std::vector<iovec> fileref_iovs;

  for (const VMArea &vma : vmas) {
    uint32_t prot = 0;
    if (vma.prot & PROT_EXEC) prot |= PROT_EXEC;
    if (vma.prot & PROT_WRITE) prot |= PROT_WRITE;
    if (vma.prot & PROT_READ) prot |= PROT_READ;

    // Skip-file-pages: send path instead of page data.
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
            fileref_segs.push_back({
                .vaddr = vma.start,
                .memsz = vma.Length(),
                .filesz = pathsz,
                .file_offset = static_cast<uint64_t>(vma.offset),
                .prot = prot,
                .type = kMigrateSegFileRef,
            });
            fileref_iovs.emplace_back(
                const_cast<char *>(path_strs.back().c_str()), pathsz);
            continue;
          }
        }
      }
    }

    size_t filesz = vma.DataLength();

    // Make memory readable if needed for transfer.
    if (filesz && !(vma.prot & PROT_READ)) {
      auto ret = KernelMProtect(reinterpret_cast<void *>(vma.start), filesz,
                                vma.prot | PROT_READ);
      if (!ret) return MakeError(ret);
    }

    // Stack trimming.
    if (vma.type == VMType::kStack && filesz) {
      size_t stack_off =
          GetStackMinOffset(reinterpret_cast<void *>(vma.start), filesz);
      uintptr_t live_start = vma.start + stack_off;
      size_t live_len = vma.Length() - stack_off;
      size_t live_filesz =
          PageAlign(GetMinSize(reinterpret_cast<void *>(live_start), live_len));
      segs.push_back({.vaddr = vma.start,
                       .memsz = vma.Length(),
                       .filesz = live_filesz,
                       .file_offset = stack_off,
                       .prot = prot,
                       .type = kMigrateSegLoad});
      if (live_filesz) {
        LOG(DEBUG) << "scatter sender: Load vaddr=0x" << std::hex << live_start
                   << " type=" << vma.TypeString() << " filesz=" << std::dec
                   << live_filesz << " memsz=" << vma.Length();
        iovs.emplace_back(reinterpret_cast<void *>(live_start), live_filesz);
      }
      continue;
    }

    // Trailing zero trimming.
    filesz = PageAlign(GetMinSize(reinterpret_cast<void *>(vma.start), filesz));

    segs.push_back({.vaddr = vma.start,
                     .memsz = vma.Length(),
                     .filesz = filesz,
                     .file_offset = 0,
                     .prot = prot,
                     .type = kMigrateSegLoad});
    if (filesz) {
      LOG(DEBUG) << "scatter sender: Load vaddr=0x" << std::hex << vma.start
                 << " type=" << vma.TypeString() << " filesz=" << std::dec
                 << filesz << " memsz=" << vma.Length();
      iovs.emplace_back(reinterpret_cast<void *>(vma.start), filesz);
    }
  }

  // FS memory areas.
  for (const FSMemoryArea &area : ctx.mem_areas_) {
    size_t saved_area = PageAlign(GetMinSize(area.ptr, area.in_use_size));
    segs.push_back({.vaddr = reinterpret_cast<uint64_t>(area.ptr),
                     .memsz = area.max_size,
                     .filesz = saved_area,
                     .file_offset = 0,
                     .prot = PROT_READ | PROT_WRITE,
                     .type = kMigrateSegLoad});
    if (saved_area) iovs.emplace_back(area.ptr, saved_area);
  }

  // Append FileRef segments after all Load segments.
  for (size_t i = 0; i < fileref_segs.size(); i++) {
    LOG(DEBUG) << "scatter sender: FileRef path="
               << std::string_view(
                      static_cast<const char *>(fileref_iovs[i].iov_base),
                      fileref_segs[i].filesz - 1)
               << " vaddr=0x" << std::hex << fileref_segs[i].vaddr
               << " memsz=" << std::dec << fileref_segs[i].memsz;
    segs.push_back(fileref_segs[i]);
    iovs.push_back(fileref_iovs[i]);
  }

  return std::make_tuple(std::move(segs), std::move(iovs),
                         std::move(path_strs));
}

// Writes scatter-copy segment data: [MigrateHeader][segments][data].
Status<void> WriteMigrateStream(MemoryMap &mm, SnapshotContext &ctx,
                                VectoredWriter &out) {
  auto ret = BuildMigrateSegments(mm, ctx);
  if (!ret) return MakeError(ret);
  auto &[segs, iovs, path_strs] = *ret;

  MigrateHeader hdr = {.segment_count = static_cast<uint32_t>(segs.size())};

  std::vector<iovec> all_iovs;
  all_iovs.reserve(2 + iovs.size());
  all_iovs.emplace_back(&hdr, sizeof(hdr));
  if (!segs.empty())
    all_iovs.emplace_back(segs.data(), segs.size() * sizeof(MigrateSegment));
  all_iovs.insert(all_iovs.end(), iovs.begin(), iovs.end());

  if (Status<void> r = WritevFull(out, all_iovs); !r) return r;

  size_t total = sizeof(hdr) + segs.size() * sizeof(MigrateSegment);
  for (const auto &iov : iovs) total += iov.iov_len;
  LOG(INFO) << "scatter migration: bytes transferred: " << total << " ("
            << (total / 1024) << " KiB)";

  return RestoreVMAProtections(mm);
}

}  // namespace

// Sender: snapshot process to scatter-copy stream.
Status<void> SnapshotProcToScatterStream(Process *p, VectoredWriter &out) {
  LOG(INFO) << "scatter-copy snapshotting proc " << p->get_pid()
            << " to stream";

  StartSnapshotContext();
  auto f = finally([] { EndSnapshotContext(); });

  Time t0 = Time::Now();
  Status<std::vector<std::byte>> metadata_buf = SerializeSnapshotMetadata(p);
  if (!metadata_buf) return MakeError(metadata_buf);
  Time t1 = Time::Now();
  LOG(INFO) << "scatter sender: serialize took " << (t1 - t0).Microseconds()
            << " us (" << metadata_buf->size() << " bytes)";

  if (Status<void> ret = WriteStreamPrefix(out, MigrationType::kScatterCopy,
                                           *metadata_buf);
      !ret)
    return ret;

  if (Status<void> ret =
          WriteMigrateStream(p->get_mem_map(), GetSnapshotContext(), out);
      !ret)
    return ret;
  Time t2 = Time::Now();
  LOG(INFO) << "scatter sender: transfer took " << (t2 - t1).Microseconds()
            << " us";
  LOG(INFO) << "scatter sender: total took " << (t2 - t0).Microseconds()
            << " us";
  return {};
}

// Receiver: restore from scatter-copy stream.
// Called after metadata deserialization; Process p is already reconstructed.
Status<std::shared_ptr<Process>> RestoreFromScatterStream(
    VectoredReader &in, std::shared_ptr<Process> p) {
  Time t0 = Time::Now();

  // Read MigrateHeader.
  MigrateHeader hdr{};
  {
    iovec iov = {&hdr, sizeof(hdr)};
    if (Status<void> ret = ReadvFull(in, {&iov, 1}); !ret)
      return MakeError(ret);
  }
  LOG(INFO) << "scatter receiver: " << hdr.segment_count << " segments";

  // Read all segment descriptors.
  std::vector<MigrateSegment> segs(hdr.segment_count);
  if (hdr.segment_count > 0) {
    iovec iov = {segs.data(), segs.size() * sizeof(MigrateSegment)};
    if (Status<void> ret = ReadvFull(in, {&iov, 1}); !ret)
      return MakeError(ret);
  }

  // Fake MemoryMap — the real one was restored by cereal.
  MemoryMap mm(nullptr, kMemoryMappingSize);
  mm.MarkAsFake();

  // Phase 1: Pre-allocate kLoad segments and build scatter-read iovecs.
  std::vector<iovec> load_iovs;
  for (const auto &seg : segs) {
    if (seg.type != kMigrateSegLoad) continue;
    if (seg.memsz == 0) continue;

    Status<void *> ret = mm.MMapAnonymous(
        reinterpret_cast<void *>(seg.vaddr), seg.memsz,
        PROT_READ | PROT_WRITE, MAP_FIXED);
    if (!ret) {
      LOG(ERR) << "scatter receiver: MMapAnonymous failed vaddr=0x" << std::hex
               << seg.vaddr << " memsz=" << std::dec << seg.memsz;
      return MakeError(ret);
    }

    if (seg.filesz > 0)
      load_iovs.emplace_back(
          reinterpret_cast<void *>(seg.vaddr + seg.file_offset), seg.filesz);
  }

  // Phase 2: Single scatter-read for all kLoad data.
  if (!load_iovs.empty()) {
    if (Status<void> ret = ReadvFull(in, load_iovs); !ret) {
      LOG(ERR) << "scatter receiver: scatter-read failed";
      return MakeError(ret);
    }
  }
  Time t1 = Time::Now();
  size_t data_bytes = 0;
  for (const auto &iov : load_iovs) data_bytes += iov.iov_len;
  LOG(INFO) << "scatter receiver: data transfer took "
            << (t1 - t0).Microseconds() << " us (" << data_bytes << " bytes, "
            << (data_bytes / 1024) << " KiB)";

  // Phase 3: Zero BSS gaps and set final protections.
  for (const auto &seg : segs) {
    if (seg.type != kMigrateSegLoad || seg.memsz == 0) continue;

    // Zero partial page between filesz and page-aligned end.
    if (seg.filesz < seg.memsz) {
      uintptr_t file_end = seg.vaddr + seg.file_offset + seg.filesz;
      uintptr_t gap_end = PageAlign(file_end);
      if (gap_end > file_end && gap_end <= seg.vaddr + seg.memsz)
        std::memset(reinterpret_cast<void *>(file_end), 0, gap_end - file_end);
    }

    // Set final protection if different from RW.
    int final_prot = static_cast<int>(seg.prot);
    if (final_prot != (PROT_READ | PROT_WRITE)) {
      Status<void> ret = mm.MProtect(reinterpret_cast<void *>(seg.vaddr),
                                     seg.memsz, final_prot);
      if (!ret) {
        LOG(ERR) << "scatter receiver: MProtect failed vaddr=0x" << std::hex
                 << seg.vaddr;
        return MakeError(ret);
      }
    }
  }

  // Phase 4: Handle kFileRef segments.
  for (const auto &seg : segs) {
    if (seg.type != kMigrateSegFileRef) continue;

    std::vector<char> pathbuf(seg.filesz);
    {
      iovec iov = {pathbuf.data(), pathbuf.size()};
      if (Status<void> ret = ReadvFull(in, {&iov, 1}); !ret)
        return MakeError(ret);
    }
    std::string_view path(pathbuf.data(), seg.filesz - 1);

    LOG(DEBUG) << "scatter receiver: FileRef path=" << path << " vaddr=0x"
               << std::hex << seg.vaddr << " memsz=" << std::dec << seg.memsz
               << " file_offset=0x" << std::hex << seg.file_offset;

    Status<JunctionFile> ref =
        JunctionFile::Open(p->get_fs(), path, 0, FileMode::kRead);
    if (!ref) {
      LOG(ERR) << "scatter receiver: FileRef failed to open " << path;
      return MakeError(ref);
    }

    int prot = static_cast<int>(seg.prot);
    if (Status<void> r = ref->MMapFixed(
            mm, reinterpret_cast<void *>(seg.vaddr), seg.memsz, prot,
            MAP_DENYWRITE, static_cast<off_t>(seg.file_offset));
        !r) {
      LOG(ERR) << "scatter receiver: FileRef MMapFixed failed for " << path;
      return MakeError(r);
    }
  }

  // Phase 5: Optionally populate pages.
  if (GetCfg().restore_populate()) {
    mm.ForEachVMA([](const VMArea &vma) {
      if (!(vma.prot & PROT_READ)) return;
      KernelMAdvise(vma.Addr(), vma.Length(), MADV_POPULATE_READ);
    });
  }

  Time t2 = Time::Now();
  LOG(INFO) << "scatter receiver: total restore took "
            << (t2 - t0).Microseconds() << " us";

  if (unlikely(GetCfg().mem_trace())) p->get_mem_map().EnableTracing(*p.get());

  timings().migration_restore_done = Time::Now();
  p->RunThreads();
  return p;
}

}  // namespace junction
