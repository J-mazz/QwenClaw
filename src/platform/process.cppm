// Copyright 2025 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0

export module quantclaw.platform.process;

import std;

export namespace quantclaw::platform {

#ifdef _WIN32
using ProcessId = unsigned long;
#else
using ProcessId = int;
#endif

constexpr ProcessId kInvalidPid = 0;

struct ExecResult {
  std::string output;
  int exit_code = -1;
  // Set when the child produced more than ExecLimits::max_output_bytes and the
  // middle of `output` was elided.
  bool output_truncated = false;
};

// Resource ceilings applied to the child between fork() and exec().
//
// These are a blast radius, not a security boundary — a child can still do
// anything the user can. Defaults are sized for real developer tooling
// (compilers, node, python), because the previous hardcoded 256 MB address
// space and 30 s CPU killed every one of them: node alone reserves far more
// virtual address space than that before running a line of script.
struct ExecLimits {
  // RLIMIT_AS. 0 disables the limit.
  std::uint64_t max_address_space_bytes = 4ULL * 1024 * 1024 * 1024;
  // RLIMIT_FSIZE. 0 disables the limit.
  std::uint64_t max_file_size_bytes = 1ULL * 1024 * 1024 * 1024;
  // RLIMIT_CPU seconds. 0 derives it from the caller's wall-clock timeout,
  // which is the only figure that scales with what the caller actually asked
  // for; a fixed value silently caps long-running tools.
  std::uint32_t max_cpu_seconds = 0;
  // Cap on captured stdout+stderr. Beyond this the middle is dropped and
  // output_truncated is set, so a chatty command cannot exhaust gateway memory
  // before the timeout fires.
  std::size_t max_output_bytes = 8ULL * 1024 * 1024;
};

ProcessId spawn_process(const std::vector<std::string>& args,
                        const std::vector<std::string>& env = {},
                        const std::string& working_dir = "");

bool is_process_alive(ProcessId pid);
void terminate_process(ProcessId pid);
void kill_process(ProcessId pid);
void reload_process(ProcessId pid);
int wait_process(ProcessId pid, int timeout_ms = -1);

ExecResult exec_capture(const std::string& command, int timeout_seconds = 30,
                        const std::string& working_dir = "",
                        const ExecLimits& limits = {});

std::string executable_path();
std::string home_directory();

}  // namespace quantclaw::platform