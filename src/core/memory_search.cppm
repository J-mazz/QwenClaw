// Copyright 2025 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0

module;

#include <spdlog/spdlog.h>

export module quantclaw.core.memory_search;

import std;
import nlohmann.json;
import quantclaw.providers.embedding_provider;
import quantclaw.core.mmr_reranker;
import quantclaw.core.temporal_decay;
import quantclaw.core.vector_index;

export namespace quantclaw {

struct MemorySearchResult {
  std::string source;
  std::string content;
  double score;
  int line_number;
};

struct HybridSearchOptions {
  double bm25_weight = 0.5;
  double vector_weight = 0.5;
  bool use_temporal_decay = true;
  bool use_mmr = true;
  double mmr_lambda = 0.7;
  int max_results = 10;
};

class MemorySearch {
 public:
  explicit MemorySearch(std::shared_ptr<spdlog::logger> logger);

  void IndexDirectory(const std::filesystem::path& dir);
  void IndexFile(const std::filesystem::path& file);
  std::vector<MemorySearchResult> Search(const std::string& query,
                                         int max_results = 10) const;
  std::vector<MemorySearchResult>
  HybridSearch(const std::string& query,
               const HybridSearchOptions& opts = {}) const;
  void SetEmbeddingProvider(std::shared_ptr<EmbeddingProvider> provider);
  void BuildVectorIndex();
  nlohmann::json Stats() const;
  void Clear();

 private:
  struct IndexEntry {
    std::string filepath;
    int line_number;
    std::string content;
    std::vector<std::string> tokens;
  };

  // One occurrence list entry: which document, and how often the term appears
  // in it. Term frequency is computed once at index time rather than
  // recomputed for every document on every query.
  struct Posting {
    std::uint32_t entry_index;
    std::uint32_t term_frequency;
  };

  static std::vector<std::string> tokenize(const std::string& text);
  void rebuild_index();

  // Body of IndexFile without the lock, so IndexDirectory can index many files
  // under a single unique_lock. mu_ is not recursive.
  void index_file_locked(const std::filesystem::path& file);
  void recompute_stats_locked();

  // Guards every member below. The memory_search tool is callable from several
  // concurrent agent turns while the workspace file watcher re-indexes, and
  // Search() scores raw IndexEntry pointers into entries_ — a push_back that
  // reallocates mid-search would leave those dangling.
  mutable std::shared_mutex mu_;

  std::shared_ptr<spdlog::logger> logger_;
  std::vector<IndexEntry> entries_;
  int total_documents_ = 0;
  double avg_doc_length_ = 0;
  static constexpr double kBM25_k1 = 1.2;
  static constexpr double kBM25_b = 0.75;

  // Inverted index: term → the documents containing it, with per-document term
  // frequency. Search visits only the documents that actually contain a query
  // term, instead of scoring all N and rebuilding each one's term-frequency
  // map from scratch. Document frequency is just postings_[term].size().
  std::unordered_map<std::string, std::vector<Posting>> postings_;
  std::shared_ptr<EmbeddingProvider> embedding_provider_;
  VectorIndex vector_index_;
  TemporalDecay temporal_decay_;
};

}  // namespace quantclaw