// Copyright 2026 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0

#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

import std;
import quantclaw.common.atomic_file;

using quantclaw::WriteFileAtomically;

namespace {

class AtomicFileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("qc_atomic_" + std::to_string(::getpid()) + "_" +
            std::to_string(counter_++));
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::string Read(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), {});
  }

  std::filesystem::path dir_;
  static inline int counter_ = 0;
};

}  // namespace

TEST_F(AtomicFileTest, CreatesFileWithContents) {
  auto p = dir_ / "new.json";
  std::string err;
  ASSERT_TRUE(WriteFileAtomically(p, "{\"a\":1}", &err)) << err;
  EXPECT_EQ(Read(p), "{\"a\":1}");
}

TEST_F(AtomicFileTest, ReplacesExistingFile) {
  auto p = dir_ / "existing.json";
  { std::ofstream(p) << "old contents that are longer"; }
  std::string err;
  ASSERT_TRUE(WriteFileAtomically(p, "new", &err)) << err;
  EXPECT_EQ(Read(p), "new") << "must not leave trailing bytes of the old file";
}

TEST_F(AtomicFileTest, CreatesMissingParentDirectories) {
  auto p = dir_ / "nested" / "deeper" / "cfg.json";
  std::string err;
  ASSERT_TRUE(WriteFileAtomically(p, "{}", &err)) << err;
  EXPECT_TRUE(std::filesystem::exists(p));
}

TEST_F(AtomicFileTest, HandlesBinaryAndEmptyContents) {
  auto p = dir_ / "bin";
  std::string payload("a\0b\0c", 5);
  std::string err;
  ASSERT_TRUE(WriteFileAtomically(p, payload, &err)) << err;
  EXPECT_EQ(Read(p), payload);

  ASSERT_TRUE(WriteFileAtomically(p, "", &err)) << err;
  EXPECT_EQ(Read(p), "");
}

// The point of the exercise: a failed write must leave the previous document
// intact rather than a truncated one.
TEST_F(AtomicFileTest, FailedWriteLeavesOriginalIntact) {
  auto p = dir_ / "keepme.json";
  const std::string original = "{\"sessions\":\"all of them\"}";
  { std::ofstream(p) << original; }

  // Make the directory read-only so the temp file cannot be created.
  ASSERT_EQ(::chmod(dir_.c_str(), 0500), 0);
  std::string err;
  bool ok = WriteFileAtomically(p, "replacement", &err);
  ASSERT_EQ(::chmod(dir_.c_str(), 0700), 0);

  EXPECT_FALSE(ok);
  EXPECT_FALSE(err.empty());
  EXPECT_EQ(Read(p), original)
      << "a truncating write would have destroyed this";
}

TEST_F(AtomicFileTest, LeavesNoTempFilesBehind) {
  auto p = dir_ / "cfg.json";
  std::string err;
  for (int i = 0; i < 20; ++i) {
    ASSERT_TRUE(WriteFileAtomically(p, "iteration " + std::to_string(i), &err))
        << err;
  }
  int entries = 0;
  for (const auto& e : std::filesystem::directory_iterator(dir_)) {
    (void)e;
    ++entries;
  }
  EXPECT_EQ(entries, 1) << "temp files must be renamed away, not accumulated";
}

TEST_F(AtomicFileTest, FailedWriteLeavesNoTempFile) {
  auto p = dir_ / "cfg.json";
  { std::ofstream(p) << "original"; }
  ASSERT_EQ(::chmod(dir_.c_str(), 0500), 0);
  std::string err;
  (void)WriteFileAtomically(p, "nope", &err);
  ASSERT_EQ(::chmod(dir_.c_str(), 0700), 0);

  int entries = 0;
  for (const auto& e : std::filesystem::directory_iterator(dir_)) {
    (void)e;
    ++entries;
  }
  EXPECT_EQ(entries, 1);
}

// Readers must observe either the whole old document or the whole new one --
// never a partial write. With a truncating ofstream this reliably catches a
// zero-length or short read; rename(2) makes it impossible.
TEST_F(AtomicFileTest, ConcurrentReadersNeverSeeAPartialDocument) {
  auto p = dir_ / "store.json";
  const std::string small(4096, 'a');
  const std::string large(512 * 1024, 'b');
  std::string err;
  ASSERT_TRUE(WriteFileAtomically(p, small, &err)) << err;

  std::atomic<bool> stop{false};
  std::atomic<int> torn{0};
  std::atomic<int> reads{0};

  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i) {
    readers.emplace_back([&] {
      while (!stop.load(std::memory_order_acquire)) {
        std::string s = Read(p);
        if (s.empty()) {
          continue;  // file momentarily absent is not the failure mode here
        }
        // Every byte must match one of the two documents, and the length must
        // be exactly one of the two lengths.
        bool valid = (s.size() == small.size() && s == small) ||
                     (s.size() == large.size() && s == large);
        if (!valid) {
          torn.fetch_add(1, std::memory_order_relaxed);
        }
        reads.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  for (int i = 0; i < 200; ++i) {
    ASSERT_TRUE(WriteFileAtomically(p, (i % 2) ? large : small, &err, false))
        << err;
  }
  stop.store(true, std::memory_order_release);
  for (auto& t : readers) {
    t.join();
  }

  EXPECT_GT(reads.load(), 0);
  EXPECT_EQ(torn.load(), 0) << "observed a partially-written document";
}
