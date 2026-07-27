// Copyright 2025 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef _WIN32

module;

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

module quantclaw.platform.process;

import std;
import quantclaw.common.defer;

namespace quantclaw::platform {

ProcessId spawn_process(const std::vector<std::string>& args,
                        const std::vector<std::string>& env,
                        const std::string& working_dir) {
  if (args.empty()) {
    return kInvalidPid;
  }

  // Everything the child needs is built BEFORE fork(). Between fork() and
  // exec() only async-signal-safe calls are legal, and in a multi-threaded
  // process that is not pedantry: the old code called setenv() there, which
  // allocates and takes a lock some other thread may have held at fork time,
  // so the child could deadlock before ever reaching exec.
  std::vector<std::string> merged_env;
  for (char** e = environ; e && *e; ++e) {
    merged_env.emplace_back(*e);
  }
  for (const auto& e : env) {
    auto eq = e.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    // Compare including the '=' so PATH_EXTRA does not shadow PATH.
    std::string key = e.substr(0, eq + 1);
    auto it = std::find_if(
        merged_env.begin(), merged_env.end(),
        [&](const std::string& existing) { return existing.starts_with(key); });
    if (it != merged_env.end()) {
      *it = e;
    } else {
      merged_env.push_back(e);
    }
  }

  std::vector<char*> c_args;
  c_args.reserve(args.size() + 1);
  for (const auto& a : args) {
    c_args.push_back(const_cast<char*>(a.c_str()));
  }
  c_args.push_back(nullptr);

  std::vector<char*> c_env;
  c_env.reserve(merged_env.size() + 1);
  for (const auto& e : merged_env) {
    c_env.push_back(const_cast<char*>(e.c_str()));
  }
  c_env.push_back(nullptr);

  pid_t pid = fork();
  if (pid < 0) {
    return kInvalidPid;
  }

  if (pid == 0) {
    // Child process — async-signal-safe calls only from here to exec.
    if (!working_dir.empty()) {
      if (chdir(working_dir.c_str()) != 0) {
        _exit(1);
      }
    }
    execvpe(c_args[0], c_args.data(), c_env.data());
    _exit(127);
  }

  return pid;
}

bool is_process_alive(ProcessId pid) {
  if (pid <= 0) {
    return false;
  }
  return kill(pid, 0) == 0;
}

void terminate_process(ProcessId pid) {
  if (pid > 0) {
    kill(pid, SIGTERM);
  }
}

void kill_process(ProcessId pid) {
  if (pid > 0) {
    kill(pid, SIGKILL);
  }
}

void reload_process(ProcessId pid) {
  if (pid > 0) {
    kill(pid, SIGHUP);
  }
}

int wait_process(ProcessId pid, int timeout_ms) {
  if (pid <= 0) {
    return -1;
  }

  if (timeout_ms == 0) {
    // Non-blocking
    int status;
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r <= 0) {
      return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  }

  if (timeout_ms < 0) {
    // Wait forever
    int status;
    pid_t r = waitpid(pid, &status, 0);
    if (r <= 0) {
      return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  }

  // Timed wait via polling
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    int status;
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r > 0) {
      return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return -1;  // Timeout
}

ExecResult exec_capture(const std::string& command, int timeout_seconds,
                        const std::string& working_dir,
                        const ExecLimits& limits) {
  ExecResult result;

  // Create a pipe for the child's stdout+stderr.
  //
  // O_CLOEXEC is load-bearing under concurrency: exec_capture runs on many
  // agent threads at once, and a plain pipe() leaks its write end into every
  // child forked by another thread in the window between our pipe() and our
  // fork(). Our read end then never sees EOF — the unrelated child holds the
  // pipe open — so the call hangs until its timeout. dup2() below clears
  // O_CLOEXEC on the descriptors the child actually needs.
  int pipefd[2];
  if (pipe2(pipefd, O_CLOEXEC) != 0) {
    result.exit_code = -1;
    return result;
  }
  // Auto-close read end at function exit (all paths).
  [[maybe_unused]] auto close_read_end =
      quantclaw::MakeDefer([&] { close(pipefd[0]); });

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[1]);  // Close write end; DEFER handles read end.
    result.exit_code = -1;
    return result;
  }

  if (pid == 0) {
    // ---- Child process ----
    close(pipefd[0]);  // close read end
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);

    // Change working directory if requested.
    if (!working_dir.empty()) {
      if (chdir(working_dir.c_str()) != 0) {
        _exit(1);
      }
    }

    // Apply resource limits in the child (not the host process).
    // Only on Linux — macOS has different rlimit semantics for some resources.
    //
    // These were previously hardcoded at 256 MB address space and 30 s CPU,
    // which is below the floor of ordinary tooling: node reserves more virtual
    // address space than that at startup, so anything JS-based died instantly,
    // and a 30 s CPU cap silently overrode any longer timeout the caller asked
    // for. They are now caller-supplied, and the CPU ceiling defaults to
    // tracking the wall-clock timeout instead of a fixed number.
#ifdef __linux__
    if (limits.max_cpu_seconds > 0 || timeout_seconds > 0) {
      rlim_t soft = limits.max_cpu_seconds > 0
                        ? static_cast<rlim_t>(limits.max_cpu_seconds)
                        : static_cast<rlim_t>(timeout_seconds);
      struct rlimit cpu_lim = {soft, soft + 30};
      if (setrlimit(RLIMIT_CPU, &cpu_lim) != 0) {
        _exit(126);  // Resource limit setup failed
      }
    }
    if (limits.max_address_space_bytes > 0) {
      rlim_t as = static_cast<rlim_t>(limits.max_address_space_bytes);
      struct rlimit mem_lim = {as, as};
      if (setrlimit(RLIMIT_AS, &mem_lim) != 0) {
        _exit(126);
      }
    }
    if (limits.max_file_size_bytes > 0) {
      rlim_t fsz = static_cast<rlim_t>(limits.max_file_size_bytes);
      struct rlimit fsz_lim = {fsz, fsz};
      if (setrlimit(RLIMIT_FSIZE, &fsz_lim) != 0) {
        _exit(126);
      }
    }
    // NOTE: RLIMIT_NPROC is intentionally NOT set here. It is a per-real-UID
    // limit counting *every* process the user owns, not just this command's
    // descendants. A small cap (e.g. 32) is exceeded on any normal interactive
    // account, so /bin/sh cannot fork the target binary and spins on EAGAIN
    // until the caller's timeout fires. Capping subprocess fan-out needs a
    // per-process-tree mechanism (cgroup pids controller), not RLIMIT_NPROC.
#endif

    execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
    _exit(127);
  }

  // ---- Parent process ----
  close(pipefd[1]);  // close write end

  auto start = std::chrono::steady_clock::now();
  auto deadline = (timeout_seconds > 0)
                      ? start + std::chrono::seconds(timeout_seconds)
                      : std::chrono::steady_clock::time_point::max();

  // 64 KiB rather than 1 KiB: pipe capacity is 64 KiB on Linux, so this reads
  // a full pipeful per syscall instead of 64 of them.
  std::array<char, 64 * 1024> buffer;
  // Head + tail ring, so a chatty command stays bounded without losing the
  // beginning (usually the command echo) or the end (usually the error).
  std::string tail;
  const std::size_t cap = limits.max_output_bytes;
  const std::size_t head_cap = cap / 2;
  bool timed_out = false;
  int child_status = -1;  // Track child exit status; -1 means not yet reaped
  bool child_reaped = false;

  // Use poll() so we never block indefinitely on read.
  struct pollfd pfd;
  pfd.fd = pipefd[0];
  pfd.events = POLLIN;

  for (;;) {
    int remaining_ms = -1;  // infinite if no timeout
    if (timeout_seconds > 0) {
      auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        timed_out = true;
        break;
      }
      remaining_ms = static_cast<int>(
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
              .count());
      if (remaining_ms <= 0) {
        timed_out = true;
        break;
      }
    }

    int pr = poll(&pfd, 1, remaining_ms);
    if (pr < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;  // error
    }
    if (pr == 0) {
      timed_out = true;
      break;  // poll timed out
    }

    ssize_t n = read(pipefd[0], buffer.data(), buffer.size());
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;  // read error
    }
    if (n == 0) {
      // EOF - child closed output, but may still be running.
      // Check if process has exited (non-blocking).
      int status;
      pid_t result_pid = waitpid(pid, &status, WNOHANG);
      if (result_pid == pid) {
        // Process exited - save status and exit loop
        child_status = status;
        child_reaped = true;
        break;
      }
      // Still running; continue loop to enforce timeout.
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    // append(ptr, n), never buffer-as-C-string: the previous code wrote a NUL
    // at buffer[n] and appended it as a char*, silently discarding everything
    // after the first embedded NUL in binary output.
    std::string_view chunk(buffer.data(), static_cast<std::size_t>(n));
    if (cap == 0) {
      result.output.append(chunk);
      continue;
    }
    if (result.output.size() < head_cap) {
      std::size_t take = std::min(chunk.size(), head_cap - result.output.size());
      result.output.append(chunk.substr(0, take));
      chunk.remove_prefix(take);
    }
    if (!chunk.empty()) {
      tail.append(chunk);
      if (tail.size() > cap - head_cap) {
        tail.erase(0, tail.size() - (cap - head_cap));
        result.output_truncated = true;
      }
    }
  }

  if (result.output_truncated) {
    result.output += "\n... [output truncated: exceeded " +
                     std::to_string(cap) + " bytes] ...\n";
  }
  result.output += tail;

  // DEFER will close pipefd[0] at function exit.

  if (timed_out) {
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    result.exit_code = -2;
    return result;
  }

  // If we haven't reaped the child yet, do it now
  if (!child_reaped) {
    waitpid(pid, &child_status, 0);
  }

  result.exit_code = WIFEXITED(child_status) ? WEXITSTATUS(child_status)
                                             : 128 + WTERMSIG(child_status);
  return result;
}

std::string executable_path() {
  char buf[4096];
  ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (len > 0) {
    buf[len] = '\0';
    return std::string(buf);
  }
  return "quantclaw";
}

std::string home_directory() {
  const char* home = std::getenv("HOME");
  return home ? home : "/tmp";
}

}  // namespace quantclaw::platform

#endif  // !_WIN32
