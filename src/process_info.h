#pragma once
// How the PTY implementations answer IPtyProcess::foreground_process().

#include "bropty/pty.h"

#include <cstdint>
#include <optional>

namespace bropty::pty_detail {

#if defined(_WIN32)
// The youngest-console-descendant chain from `root_pid`, which was created at
// `root_created` (a FILETIME as 100 ns ticks; a process that claims the root
// as parent but is older than it is a stranger holding a recycled pid).
std::optional<ProcessInfo> foreground_of_tree(int64_t root_pid, uint64_t root_created);
#else
// The foreground process group of the terminal whose master is `master_fd`.
std::optional<ProcessInfo> foreground_of_tty(int master_fd);
#endif

} // namespace bropty::pty_detail
