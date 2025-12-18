
#pragma once

#include <memory>

#include "junction/base/error.h"

namespace junction {

class Process;

Status<void> SetupServerlessChannel(const std::string &name);
void WarmupAndSnapshot(std::shared_ptr<Process> proc, const std::string &name,
                       std::string_view arg);

pid_t GetLastBlockedTid(std::string &name);
std::string InvokeChan(std::string &name, std::string arg);
void RunRestored(std::shared_ptr<Process> proc, const std::string &name,
                 std::string_view arg);
}  // namespace junction
