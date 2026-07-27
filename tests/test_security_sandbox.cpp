// Copyright 2025 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0

#include <filesystem>
#include <memory>

#include <gtest/gtest.h>
import quantclaw.platform.process;
import quantclaw.security.sandbox;

import quantclaw.test.helpers;

#ifdef __linux__
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

import std;
#endif

class SandboxTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_dir_ = quantclaw::test::MakeTestDir("quantclaw_sandbox_test");
  }

  void TearDown() override {
    if (std::filesystem::exists(test_dir_)) {
      std::filesystem::remove_all(test_dir_);
    }
  }

  std::filesystem::path test_dir_;
};

TEST_F(SandboxTest, AllowedPathWithinWorkspace) {
  quantclaw::Sandbox sandbox(test_dir_, {test_dir_.string()},  // allowed
                             {},                               // denied
                             {},  // allowed commands
                             {}   // denied commands
  );

  auto file_in_workspace = test_dir_ / "SOUL.md";
  EXPECT_TRUE(sandbox.IsPathAllowed(file_in_workspace.string()));
}

TEST_F(SandboxTest, DeniedPathOutsideWorkspace) {
  quantclaw::Sandbox sandbox(test_dir_, {test_dir_.string()},  // allowed
                             {},                               // denied
                             {}, {});

  EXPECT_FALSE(sandbox.IsPathAllowed("/etc/passwd"));
}

TEST_F(SandboxTest, ExplicitDenyOverridesAllow) {
  quantclaw::Sandbox sandbox(test_dir_, {"/"},  // allow everything
                             {"/etc"},          // but deny /etc
                             {}, {});

  EXPECT_FALSE(sandbox.IsPathAllowed("/etc/passwd"));
  EXPECT_TRUE(sandbox.IsPathAllowed("/tmp/test.txt"));
}

TEST_F(SandboxTest, EmptyAllowedPathsPermitsAll) {
  quantclaw::Sandbox sandbox(
      test_dir_, {},  // no allowed paths → permit all (except denied)
      {}, {}, {});

  EXPECT_TRUE(sandbox.IsPathAllowed("/tmp/anything"));
}

TEST_F(SandboxTest, SanitizePathTraversal) {
  quantclaw::Sandbox sandbox(test_dir_, {}, {}, {}, {});

  EXPECT_THROW(sandbox.SanitizePath("../../../etc/passwd"), std::runtime_error);
}

TEST_F(SandboxTest, SanitizeNormalPath) {
  quantclaw::Sandbox sandbox(test_dir_, {}, {}, {}, {});

  auto result = sandbox.SanitizePath(test_dir_.string() + "/SOUL.md");
  EXPECT_FALSE(result.empty());
}

// --- Static validators ---

TEST_F(SandboxTest, ValidateFilePath) {
  EXPECT_TRUE(quantclaw::Sandbox::ValidateFilePath("/tmp/test.txt", "/tmp"));
  EXPECT_TRUE(
      quantclaw::Sandbox::ValidateFilePath("/tmp/sub/dir/file.txt", "/tmp"));
  EXPECT_FALSE(
      quantclaw::Sandbox::ValidateFilePath("../../etc/passwd", "/tmp"));
  // Absolute path outside workspace must be rejected.
  EXPECT_FALSE(quantclaw::Sandbox::ValidateFilePath("/etc/passwd", "/tmp"));
#ifdef _WIN32
  EXPECT_FALSE(quantclaw::Sandbox::ValidateFilePath(
      "C:\\Windows\\win.ini", "C:\\Users\\test\\workspace"));
  EXPECT_TRUE(quantclaw::Sandbox::ValidateFilePath(
      "C:\\Users\\test\\workspace\\file.txt", "C:\\Users\\test\\workspace"));
#endif
}

TEST_F(SandboxTest, ValidateShellCommandSafe) {
  EXPECT_TRUE(quantclaw::Sandbox::ValidateShellCommand("ls -la"));
  EXPECT_TRUE(quantclaw::Sandbox::ValidateShellCommand("echo hello"));
}

TEST_F(SandboxTest, ValidateShellCommandDangerous) {
  EXPECT_FALSE(quantclaw::Sandbox::ValidateShellCommand("rm -rf /"));
  EXPECT_FALSE(
      quantclaw::Sandbox::ValidateShellCommand("dd if=/dev/zero of=/dev/sda"));
  EXPECT_FALSE(quantclaw::Sandbox::ValidateShellCommand("mkfs.ext4 /dev/sda"));
}

// --- Command filtering ---

TEST_F(SandboxTest, DenyCommandByPattern) {
  quantclaw::Sandbox sandbox(test_dir_, {}, {}, {}, {"rm\\s+-rf"}
                             // denied command pattern (regex)
  );

  EXPECT_FALSE(sandbox.IsCommandAllowed("rm -rf /"));
  EXPECT_TRUE(sandbox.IsCommandAllowed("ls -la"));
}

// --- Resource limits ---

TEST_F(SandboxTest, ApplyResourceLimitsDoesNotThrow) {
  // ApplyResourceLimits is now intentionally a no-op (resource limits are
  // applied inside exec_capture on the child process). Just verify it
  // doesn't throw.
  EXPECT_NO_THROW(quantclaw::Sandbox::ApplyResourceLimits());
}

#ifdef __linux__
namespace {
std::string trimmed(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == ' '))
    s.pop_back();
  return s;
}
}  // namespace

// Limits are applied to the child spawned by exec_capture, never to the host
// process — capping the gateway itself would be permanent.
//
// The CPU ceiling now follows the caller's wall-clock timeout instead of a
// fixed 30 s. The old constant silently overrode any longer timeout a caller
// asked for, which is what made the Claude advisor tool (300 s) unusable.
TEST_F(SandboxTest, CpuLimitInChildTracksCallerTimeout) {
  auto result = quantclaw::platform::exec_capture("ulimit -t", 7);
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_EQ(trimmed(result.output), "7");

  auto longer = quantclaw::platform::exec_capture("ulimit -t", 120);
  EXPECT_EQ(longer.exit_code, 0);
  EXPECT_EQ(trimmed(longer.output), "120")
      << "a longer timeout must not be capped at the old fixed ceiling";
}

TEST_F(SandboxTest, ExplicitCpuLimitOverridesTimeout) {
  quantclaw::platform::ExecLimits limits;
  limits.max_cpu_seconds = 45;
  auto result = quantclaw::platform::exec_capture("ulimit -t", 10, "", limits);
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_EQ(trimmed(result.output), "45");
}

TEST_F(SandboxTest, AddressSpaceLimitIsAppliedAndDisablable) {
  quantclaw::platform::ExecLimits limits;
  limits.max_address_space_bytes = 2ULL * 1024 * 1024 * 1024;
  auto capped = quantclaw::platform::exec_capture("ulimit -v", 5, "", limits);
  EXPECT_EQ(capped.exit_code, 0);
  EXPECT_EQ(trimmed(capped.output), "2097152");  // reported in KiB

  // 0 means "no limit" — required for Node-based tools such as the Claude CLI,
  // which reserve more virtual address space than any sane cap.
  limits.max_address_space_bytes = 0;
  auto uncapped = quantclaw::platform::exec_capture("ulimit -v", 5, "", limits);
  EXPECT_EQ(uncapped.exit_code, 0);
  EXPECT_EQ(trimmed(uncapped.output), "unlimited");
}

// The host process must never inherit the child's ceilings.
TEST_F(SandboxTest, HostProcessLimitsUnchangedByExecCapture) {
  struct rlimit before {};
  ASSERT_EQ(getrlimit(RLIMIT_CPU, &before), 0);
  (void)quantclaw::platform::exec_capture("true", 5);
  struct rlimit after {};
  ASSERT_EQ(getrlimit(RLIMIT_CPU, &after), 0);
  EXPECT_EQ(before.rlim_cur, after.rlim_cur);
  EXPECT_EQ(before.rlim_max, after.rlim_max);
}
#endif
