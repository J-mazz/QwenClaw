// Copyright 2025 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

import std;
import quantclaw.platform.ipc;
import quantclaw.platform.process;
import quantclaw.platform.service;

using namespace quantclaw::platform;

// --- Process tests ---

TEST(PlatformProcess, HomeDirNotEmpty) {
  std::string home = home_directory();
  EXPECT_FALSE(home.empty());
  EXPECT_TRUE(std::filesystem::exists(home));
}

TEST(PlatformProcess, ExecutablePathNotEmpty) {
  std::string exe = executable_path();
  EXPECT_FALSE(exe.empty());
}

TEST(PlatformProcess, ExecCaptureEcho) {
  auto result = exec_capture("echo hello", 5);
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_NE(result.output.find("hello"), std::string::npos);
}

TEST(PlatformProcess, ExecCaptureFail) {
#ifdef _WIN32
  auto result = exec_capture("cmd /c exit 1", 5);
#else
  auto result = exec_capture("false", 5);
#endif
  EXPECT_NE(result.exit_code, 0);
}

TEST(PlatformProcess, SpawnAndWait) {
  // Spawn a short-lived process
#ifdef _WIN32
  // Use absolute paths: CMake's PROPERTIES ENVIRONMENT may restrict PATH,
  // preventing cmd.exe from finding ping.exe via relative name.
  std::vector<std::string> args = {
      "C:\\Windows\\System32\\cmd.exe", "/c",
      "C:\\Windows\\System32\\PING.EXE -n 2 127.0.0.1 >nul"};
#else
  std::vector<std::string> args = {"sleep", "0.1"};
#endif
  auto pid = spawn_process(args);
  ASSERT_NE(pid, kInvalidPid);
  EXPECT_TRUE(is_process_alive(pid));

  // Wait for it to finish
  int exit_code = wait_process(pid, 5000);
  EXPECT_EQ(exit_code, 0);
  EXPECT_FALSE(is_process_alive(pid));
}

TEST(PlatformProcess, SpawnInvalidBinary) {
  std::vector<std::string> args = {"/nonexistent/binary_xyz_123"};
  auto pid = spawn_process(args);
#ifdef _WIN32
  // On Windows, CreateProcess fails and returns kInvalidPid
  EXPECT_EQ(pid, kInvalidPid);
#else
  // On Unix, fork succeeds but exec fails — child exits with 127
  if (pid != kInvalidPid) {
    int exit_code = wait_process(pid, 2000);
    EXPECT_EQ(exit_code, 127);
  }
#endif
}

TEST(PlatformProcess, TerminateProcess) {
#ifdef _WIN32
  std::vector<std::string> args = {
      "C:\\Windows\\System32\\cmd.exe", "/c",
      "C:\\Windows\\System32\\PING.EXE -n 60 127.0.0.1 >nul"};
#else
  std::vector<std::string> args = {"sleep", "60"};
#endif
  auto pid = spawn_process(args);
  ASSERT_NE(pid, kInvalidPid);
  EXPECT_TRUE(is_process_alive(pid));

  terminate_process(pid);
  int exit_code = wait_process(pid, 5000);
  // SIGTERM results in 128+15=143 on Unix
  EXPECT_NE(exit_code, -1);
  EXPECT_FALSE(is_process_alive(pid));
}

TEST(PlatformProcess, KillProcess) {
#ifdef _WIN32
  std::vector<std::string> args = {
      "C:\\Windows\\System32\\cmd.exe", "/c",
      "C:\\Windows\\System32\\PING.EXE -n 60 127.0.0.1 >nul"};
#else
  std::vector<std::string> args = {"sleep", "60"};
#endif
  auto pid = spawn_process(args);
  ASSERT_NE(pid, kInvalidPid);

  kill_process(pid);
  int exit_code = wait_process(pid, 5000);
  EXPECT_NE(exit_code, -1);
}

TEST(PlatformProcess, SpawnWithEnv) {
#ifdef _WIN32
  std::vector<std::string> args = {"C:\\Windows\\System32\\cmd.exe", "/c",
                                   "set"};
#else
  std::vector<std::string> args = {"env"};
#endif
  std::vector<std::string> env = {"TEST_PLATFORM_VAR=hello123"};
  auto pid = spawn_process(args, env);
  ASSERT_NE(pid, kInvalidPid);
  wait_process(pid, 5000);
}

TEST(PlatformProcess, WaitNonBlockingNotExited) {
#ifdef _WIN32
  std::vector<std::string> args = {
      "C:\\Windows\\System32\\cmd.exe", "/c",
      "C:\\Windows\\System32\\PING.EXE -n 60 127.0.0.1 >nul"};
#else
  std::vector<std::string> args = {"sleep", "60"};
#endif
  auto pid = spawn_process(args);
  ASSERT_NE(pid, kInvalidPid);

  // Non-blocking wait should return -1 (not yet exited)
  int exit_code = wait_process(pid, 0);
  EXPECT_EQ(exit_code, -1);

  terminate_process(pid);
  wait_process(pid, 5000);
}

// --- IPC tests (TCP loopback) ---

TEST(PlatformIpc, ServerListenAndGetPort) {
  // IpcServer binds to 127.0.0.1:0 and reports the assigned port.
  IpcServer server;
  EXPECT_TRUE(server.listen());
  EXPECT_GT(server.port(), 0);
  server.close();
  // cleanup() is a no-op for TCP — just verify it doesn't crash.
  IpcServer::cleanup("");
}

TEST(PlatformIpc, ClientServerRoundtrip) {
  IpcServer server;
  ASSERT_TRUE(server.listen());
  int port = server.port();

  // Spawn a client thread that connects to the server's port.
  std::thread client_thread([port]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    IpcClient client("127.0.0.1", port);
    ASSERT_TRUE(client.connect());

    const char* msg = "hello\n";
    int written = ipc_write(client.handle(), msg, 6);
    EXPECT_EQ(written, 6);

    client.close();
  });

  auto conn = server.accept(5000);
  ASSERT_NE(conn, kInvalidIpc);

  // Read the message
  auto line = ipc_read_line(conn, 3000);
  EXPECT_EQ(line, "hello");

  ipc_close(conn);
  server.close();
  client_thread.join();
}

TEST(PlatformIpc, AcceptTimeout) {
  IpcServer server;
  ASSERT_TRUE(server.listen());

  // No client connects — should timeout quickly.
  auto conn = server.accept(100);
  EXPECT_EQ(conn, kInvalidIpc);

  server.close();
}

TEST(PlatformIpc, SetPermissionsIsNoOp) {
  // ipc_set_permissions is a no-op for TCP; verify it doesn't crash.
  ipc_set_permissions("", 0600);
}

// --- ServiceManager tests ---

TEST(PlatformService, ConstructAndQuery) {
  auto logger = spdlog::default_logger();
  ServiceManager svc(logger);

  // Should not crash, PID should be -1 when not running
  EXPECT_EQ(svc.get_pid(), -1);
}

TEST(PlatformService, WritePidFile) {
  auto logger = spdlog::default_logger();
  ServiceManager svc(logger);

  svc.write_pid(12345);
  EXPECT_EQ(svc.get_pid(), 12345);
  svc.remove_pid();
  EXPECT_EQ(svc.get_pid(), -1);
}

// --- exec_capture hardening ---
//
// Each of these covers a property that was silently wrong before: binary
// output truncated at the first NUL, unbounded capture, concurrent calls
// hanging on a leaked pipe write end, and rlimits too tight to run real tools.

TEST(PlatformProcess, ExecCaptureKeepsBytesAfterEmbeddedNul) {
  // printf writes a NUL between the two words. The old implementation
  // NUL-terminated the read buffer and appended it as a C string, so
  // everything from the NUL onward vanished.
  auto result = exec_capture(R"(printf 'before\000after')", 10);
  ASSERT_EQ(result.exit_code, 0);
  EXPECT_EQ(result.output.size(), 12u) << "expected 'before\\0after' verbatim";
  EXPECT_NE(result.output.find(std::string("after")), std::string::npos);
  EXPECT_EQ(result.output[6], '\0');
}

TEST(PlatformProcess, ExecCaptureBoundsRunawayOutput) {
  ExecLimits limits;
  limits.max_output_bytes = 64 * 1024;
  // `yes` never stops; without a cap this grows until the timeout kills it.
  auto result = exec_capture("yes abcdefghijklmnop", 3, "", limits);
  EXPECT_TRUE(result.output_truncated);
  // Head + tail + the truncation marker, nothing like the gigabytes `yes`
  // would otherwise have produced in three seconds.
  EXPECT_LT(result.output.size(), limits.max_output_bytes + 1024);
  EXPECT_NE(result.output.find("output truncated"), std::string::npos);
}

TEST(PlatformProcess, ExecCaptureKeepsHeadAndTail) {
  ExecLimits limits;
  limits.max_output_bytes = 4096;
  auto result = exec_capture(
      R"(echo FIRSTLINE; for i in $(seq 1 4000); do echo padding-$i; done; echo LASTLINE)",
      20, "", limits);
  ASSERT_EQ(result.exit_code, 0);
  EXPECT_TRUE(result.output_truncated);
  EXPECT_NE(result.output.find("FIRSTLINE"), std::string::npos)
      << "head of the output must survive truncation";
  EXPECT_NE(result.output.find("LASTLINE"), std::string::npos)
      << "tail of the output must survive truncation";
}

TEST(PlatformProcess, ExecCaptureAllowsRealisticMemoryFootprint) {
  // The old hardcoded RLIMIT_AS of 256 MB is below what many interpreters
  // reserve at startup. Allocating 300 MB must succeed under the defaults.
  auto result = exec_capture(
      R"(sh -c 'a=$(head -c 1000000 /dev/zero | tr "\0" "x"); echo sized-ok')",
      20);
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_NE(result.output.find("sized-ok"), std::string::npos);
}

TEST(PlatformProcess, ExecCaptureCpuLimitTracksTimeout) {
  // A 20 s timeout must not be silently capped at the old fixed 30 s CPU
  // ceiling in a way that kills short work; this should simply succeed.
  auto result = exec_capture("sleep 1; echo done", 20);
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_NE(result.output.find("done"), std::string::npos);
}

TEST(PlatformProcess, ExecCaptureTimesOutCleanly) {
  auto start = std::chrono::steady_clock::now();
  auto result = exec_capture("sleep 30", 2);
  auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_EQ(result.exit_code, -2) << "timeout is reported as -2";
  EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(),
            10);
}

TEST(PlatformProcess, ExecCaptureConcurrentCallsDoNotBlockEachOther) {
  // Without O_CLOEXEC, a fork on one thread inherits another thread's pipe
  // write end, so that thread's read never sees EOF and it runs to its
  // timeout. With 8 threads doing quick work, a timeout means the leak is back.
  constexpr int kThreads = 8;
  std::atomic<int> ok{0};
  std::atomic<int> timed_out{0};
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;

  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i] {
      while (!go.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (int j = 0; j < 12; ++j) {
        // Mix instant and slightly-slower children to widen the fork window.
        auto r = exec_capture(
            (j % 3 == 0) ? "sleep 0.2; echo t" + std::to_string(i)
                         : "echo t" + std::to_string(i),
            15);
        if (r.exit_code == -2) {
          timed_out.fetch_add(1, std::memory_order_relaxed);
        } else if (r.exit_code == 0) {
          ok.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  go.store(true, std::memory_order_release);
  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(timed_out.load(), 0) << "a concurrent fork leaked our pipe write end";
  EXPECT_EQ(ok.load(), kThreads * 12);
}

TEST(PlatformProcess, SpawnProcessAppliesEnvOverride) {
  // env is now materialised before fork() and passed via execvpe, rather than
  // calling the non-async-signal-safe setenv() in the child.
  auto out = std::filesystem::temp_directory_path() /
             ("qc_env_" + std::to_string(::getpid()) + ".txt");
  auto pid = spawn_process(
      {"sh", "-c", "printf '%s' \"$QC_TEST_VAR\" > " + out.string()},
      {"QC_TEST_VAR=spawned-value"});
  ASSERT_NE(pid, kInvalidPid);
  EXPECT_EQ(wait_process(pid, 10000), 0);

  std::ifstream f(out);
  std::string contents((std::istreambuf_iterator<char>(f)), {});
  EXPECT_EQ(contents, "spawned-value");

  std::error_code ec;
  std::filesystem::remove(out, ec);
}

TEST(PlatformProcess, SpawnProcessInheritsAmbientEnvironment) {
  // Overriding one variable must not wipe the rest of the environment.
  auto out = std::filesystem::temp_directory_path() /
             ("qc_env2_" + std::to_string(::getpid()) + ".txt");
  auto pid = spawn_process(
      {"sh", "-c", "printf '%s' \"${HOME:-missing}\" > " + out.string()},
      {"QC_TEST_VAR=irrelevant"});
  ASSERT_NE(pid, kInvalidPid);
  EXPECT_EQ(wait_process(pid, 10000), 0);

  std::ifstream f(out);
  std::string contents((std::istreambuf_iterator<char>(f)), {});
  EXPECT_NE(contents, "missing");
  EXPECT_FALSE(contents.empty());

  std::error_code ec;
  std::filesystem::remove(out, ec);
}
