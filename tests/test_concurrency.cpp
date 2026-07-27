// Copyright 2026 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0
//
// Concurrency stress tests for the registries that agent threads share.
//
// The gateway serves many sessions at once: agent turns resolve providers,
// execute tools and search memory from their own threads, while MCP
// reconnects re-register tools and the workspace watcher re-indexes files.
// Each container below was previously unsynchronised, so a re-registration or
// re-index concurrent with a read was a data race — and in MemorySearch's
// case a use-after-free, because Search() scores raw pointers into a vector
// that push_back can reallocate.
//
// These are stress tests, not proofs: they hammer the real access patterns and
// reliably fault on the unsynchronised versions. Run under ThreadSanitizer
// (cmake -DENABLE_TSAN=ON, requires the libtsan runtime) for an actual
// happens-before check.

#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <unistd.h>

#include <gtest/gtest.h>

import std;
import nlohmann.json;
import quantclaw.core.memory_search;
import quantclaw.providers.provider_registry;
import quantclaw.tools.tool_registry;
import quantclaw.providers.llm_provider;

using namespace quantclaw;

namespace {

std::shared_ptr<spdlog::logger> quiet_logger() {
  return std::make_shared<spdlog::logger>(
      "conc", std::make_shared<spdlog::sinks::null_sink_mt>());
}

// Run `fn(i)` on `n` threads, released together to maximise overlap.
template <typename F>
void RunConcurrently(int n, F&& fn) {
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  threads.reserve(n);
  for (int i = 0; i < n; ++i) {
    threads.emplace_back([&, i] {
      while (!go.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      fn(i);
    });
  }
  go.store(true, std::memory_order_release);
  for (auto& t : threads) {
    t.join();
  }
}

constexpr int kThreads = 8;
constexpr int kIters = 200;

}  // namespace

// ── ToolRegistry ─────────────────────────────────────────────────────────────

// Readers must survive an MCP server registering tools underneath them.
TEST(ConcurrencyTest, ToolRegistryReadsDuringExternalRegistration) {
  ToolRegistry registry(quiet_logger());
  registry.RegisterBuiltinTools();

  std::atomic<int> executed{0};

  RunConcurrently(kThreads, [&](int id) {
    if (id == 0) {
      // Writer: an MCP connection coming up over and over.
      for (int i = 0; i < kIters; ++i) {
        registry.RegisterExternalTool(
            "mcp_tool_" + std::to_string(i % 16), "external",
            nlohmann::json{{"type", "object"}},
            [](const nlohmann::json&) -> std::string { return "ok"; });
      }
      return;
    }
    for (int i = 0; i < kIters; ++i) {
      auto schemas = registry.GetToolSchemas();
      EXPECT_FALSE(schemas.empty());
      (void)registry.HasTool("read");
      // A side-effect-free builtin, so the loop exercises dispatch rather
      // than the filesystem.
      try {
        registry.ExecuteTool(
            "message", {{"channel", "test"}, {"message", "ping"}});
        executed.fetch_add(1, std::memory_order_relaxed);
      } catch (const std::exception&) {
        // Tool-level failures are fine; a crash or corrupted map is not.
      }
    }
  });

  EXPECT_GT(executed.load(), 0);
  EXPECT_TRUE(registry.HasTool("read"));
}

// The handler is copied out before the lock is released, so a tool being
// re-registered mid-call cannot pull the std::function out from under it.
TEST(ConcurrencyTest, ToolRegistryExecuteSurvivesReregistration) {
  ToolRegistry registry(quiet_logger());
  registry.RegisterBuiltinTools();

  std::atomic<bool> stop{false};
  std::thread churn([&] {
    while (!stop.load(std::memory_order_acquire)) {
      registry.RegisterExternalTool(
          "churn", "replaced repeatedly", nlohmann::json{{"type", "object"}},
          [](const nlohmann::json&) -> std::string {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
            return "handler-result";
          });
    }
  });

  int ok = 0;
  for (int i = 0; i < 500; ++i) {
    try {
      if (registry.ExecuteTool("churn", nlohmann::json::object()) ==
          "handler-result") {
        ++ok;
      }
    } catch (const std::exception&) {
      // "Tool not found" is legal only before the first registration.
    }
  }
  stop.store(true, std::memory_order_release);
  churn.join();

  EXPECT_GT(ok, 0);
}

// ── ProviderRegistry ─────────────────────────────────────────────────────────

// GetProvider memoises into instances_ on the model-resolution path, which
// every agent turn hits. Concurrent first-touch must yield one shared instance.
TEST(ConcurrencyTest, ProviderRegistryMemoisesUnderContention) {
  ProviderRegistry registry(quiet_logger());
  registry.RegisterBuiltinFactories();

  ProviderEntry entry;
  entry.id = "local";
  registry.AddProvider(entry);

  std::mutex seen_mu;
  std::set<LLMProvider*> seen;

  RunConcurrently(kThreads, [&](int) {
    for (int i = 0; i < kIters; ++i) {
      auto p = registry.GetProvider("local");
      ASSERT_NE(p, nullptr);
      std::lock_guard<std::mutex> lk(seen_mu);
      seen.insert(p.get());
    }
  });

  // Memoisation happens under the lock, so every thread sees the same object.
  EXPECT_EQ(seen.size(), 1u);
}

// GetProvider also inserts into entries_ for ids it has no entry for; doing
// that from several threads at once must not corrupt the map.
TEST(ConcurrencyTest, ProviderRegistryConcurrentImplicitEntryCreation) {
  ProviderRegistry registry(quiet_logger());
  registry.RegisterBuiltinFactories();

  RunConcurrently(kThreads, [&](int) {
    for (int i = 0; i < 50; ++i) {
      (void)registry.GetProvider("local");
      (void)registry.GetProvider("anthropic");
      (void)registry.HasProvider("local");
      (void)registry.ProviderIds();
    }
  });

  EXPECT_TRUE(registry.HasProvider("local"));
  EXPECT_NE(registry.GetProvider("local"), nullptr);
}

// ── MemorySearch ─────────────────────────────────────────────────────────────

// Search() holds raw IndexEntry pointers while scoring. Re-indexing from the
// file watcher push_backs into that same vector; before the lock, a
// reallocation mid-search was a use-after-free.
TEST(ConcurrencyTest, MemorySearchReadsDuringReindex) {
  auto dir = std::filesystem::temp_directory_path() /
             ("qc_conc_" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  for (int i = 0; i < 8; ++i) {
    std::ofstream f(dir / ("doc" + std::to_string(i) + ".md"));
    f << "alpha beta gamma document " << i << "\n\n"
      << "delta epsilon searchable content " << i << "\n";
  }

  MemorySearch search(quiet_logger());
  search.IndexDirectory(dir);

  RunConcurrently(kThreads, [&](int id) {
    if (id == 0) {
      for (int i = 0; i < 40; ++i) {
        search.IndexFile(dir / ("doc" + std::to_string(i % 8) + ".md"));
      }
      return;
    }
    for (int i = 0; i < kIters; ++i) {
      auto results = search.Search("alpha searchable", 5);
      for (const auto& r : results) {
        // Touch the payload: a dangling entry pointer shows up here.
        EXPECT_FALSE(r.source.empty());
        EXPECT_FALSE(r.content.empty());
      }
      (void)search.Stats();
    }
  });

  EXPECT_FALSE(search.Search("alpha", 3).empty());
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

// Clear() while searches are in flight is the most destructive interleaving.
TEST(ConcurrencyTest, MemorySearchSearchDuringClear) {
  auto dir = std::filesystem::temp_directory_path() /
             ("qc_conc_clear_" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  {
    std::ofstream f(dir / "notes.md");
    f << "alpha beta gamma\n\ndelta epsilon zeta\n";
  }

  MemorySearch search(quiet_logger());
  search.IndexDirectory(dir);

  RunConcurrently(4, [&](int id) {
    if (id == 0) {
      for (int i = 0; i < 100; ++i) {
        search.Clear();
        search.IndexFile(dir / "notes.md");
      }
      return;
    }
    for (int i = 0; i < kIters; ++i) {
      for (const auto& r : search.Search("alpha delta", 5)) {
        EXPECT_FALSE(r.content.empty());
      }
    }
  });

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  SUCCEED();
}
