// migrate_format.h - wire format for scatter-copy migration

#pragma once

#include <cstdint>

namespace junction {

#pragma pack(push, 1)

struct MigrateHeader {
  uint32_t segment_count;
};

enum : uint32_t {
  kMigrateSegLoad = 1,
  kMigrateSegFileRef = 2,
};

struct MigrateSegment {
  uint64_t vaddr;        // target virtual address
  uint64_t memsz;        // total memory region size
  uint64_t filesz;       // bytes in stream (page data for Load, path len for FileRef)
  uint64_t file_offset;  // for FileRef: offset into backing file; unused for Load
  uint32_t prot;         // PROT_READ | PROT_WRITE | PROT_EXEC
  uint32_t type;         // kMigrateSegLoad or kMigrateSegFileRef
};

#pragma pack(pop)

}  // namespace junction
