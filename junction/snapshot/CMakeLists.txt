message(STATUS "Building junction snapshot")

add_library(snapshot
  jif.cc
  snapshot.cc
  snapshot_elf.cc
  snapshot_jif.cc
  snapshot_scatter.cc
)

target_link_libraries(snapshot
  base
  bindings
  kernel
)
