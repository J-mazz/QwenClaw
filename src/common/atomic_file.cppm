// Copyright 2026 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0
//
// Crash-safe whole-file replacement.
//
// The gateway rewrites its config document and its session index in place with
// a truncating ofstream. That has a window in which the file exists but is
// empty or half-written: a crash, an OOM kill, or a power loss inside it does
// not corrupt one record, it destroys the whole document. For the session
// index that is every session the user has; for the config it is every
// provider and key they configured. The session index was additionally
// rewritten on *every appended message*, so the window was open constantly.
//
// The fix is the standard durable-replace dance: write a sibling temp file,
// fsync it, rename it over the target (rename(2) is atomic within a
// filesystem), then fsync the directory so the rename itself is durable.
// Readers see either the old file or the new one, never a partial one.

module;

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

export module quantclaw.common.atomic_file;

import std;

export namespace quantclaw {

// Replace `path` with `contents` atomically. Returns false and sets `error`
// on failure, leaving any existing file untouched.
//
// `durable` fsyncs the data and the containing directory before returning.
// That survives a power loss but costs a disk round-trip, so callers on a hot
// path (e.g. a debounced session-index flush) may pass false: the rename is
// still atomic with respect to other processes and to a process crash, only
// an OS-level crash can then lose the most recent write.
bool WriteFileAtomically(const std::filesystem::path& path,
                         std::string_view contents, std::string* error = nullptr,
                         bool durable = true) {
  auto fail = [&](std::string_view what) {
    if (error) {
      *error = std::string(what) + ": " + std::strerror(errno);
    }
    return false;
  };

  std::error_code ec;
  auto dir = path.parent_path();
  if (!dir.empty()) {
    std::filesystem::create_directories(dir, ec);
    if (ec) {
      if (error) {
        *error = "create_directories failed: " + ec.message();
      }
      return false;
    }
  } else {
    dir = ".";
  }

  // Temp file must be a sibling: rename(2) is only atomic within one
  // filesystem, so /tmp is not a safe staging area.
  std::string tmpl = (dir / (".{}.tmpXXXXXX")).string();
  auto pos = tmpl.find("{}");
  tmpl.replace(pos, 2, path.filename().string());
  std::vector<char> tmp_path(tmpl.begin(), tmpl.end());
  tmp_path.push_back('\0');

  int fd = ::mkstemp(tmp_path.data());
  if (fd < 0) {
    return fail("mkstemp failed");
  }
  std::filesystem::path tmp(tmp_path.data());

  // Clean up the temp file on every failure path below.
  bool committed = false;
  struct Cleanup {
    const std::filesystem::path& p;
    const bool& done;
    ~Cleanup() {
      if (!done) {
        std::error_code e;
        std::filesystem::remove(p, e);
      }
    }
  } cleanup{tmp, committed};

  std::size_t written = 0;
  while (written < contents.size()) {
    ssize_t n = ::write(fd, contents.data() + written, contents.size() - written);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      ::close(fd);
      return fail("write failed");
    }
    written += static_cast<std::size_t>(n);
  }

  if (durable && ::fsync(fd) != 0) {
    ::close(fd);
    return fail("fsync failed");
  }
  if (::close(fd) != 0) {
    return fail("close failed");
  }

  // mkstemp creates 0600; match what a plain ofstream would have produced so
  // replacing a file does not silently tighten or loosen its permissions.
  std::filesystem::permissions(
      tmp,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
          std::filesystem::perms::group_read |
          std::filesystem::perms::others_read,
      ec);

  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    return fail("rename failed");
  }
  committed = true;

  if (durable) {
    // Without this the rename can still be lost on power failure even though
    // the data was fsynced: the directory entry is its own metadata.
    int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
      ::fsync(dfd);
      ::close(dfd);
    }
  }
  return true;
}

}  // namespace quantclaw
