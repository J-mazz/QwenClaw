// Copyright 2025 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0

module;

#include <spdlog/spdlog.h>

module quantclaw.providers.provider_registry;

import std;

import nlohmann.json;

import quantclaw.providers.anthropic_provider;
import quantclaw.providers.llama_provider;
import quantclaw.providers.llm_provider;

namespace quantclaw {

// NOTE: this implementation unit deliberately holds ONLY the members that are
// not defined in the module interface (provider_registry.cppm).
//
// It previously carried a second, full copy of 16 more methods. Those copies
// were dead: GCC emits the interface unit's definitions and drops the
// duplicates here, so provider_registry.cpp.o exported exactly the two
// functions below and nothing else. Edits to the other copies silently did
// nothing -- and one had in fact diverged, defaulting an empty Anthropic
// api_key to "local", a behaviour change that never once executed.
//
// Keep new ProviderRegistry members in ONE place. Verify with:
//   nm -C --defined-only <build>/…/provider_registry.cpp.o

std::vector<ProviderRegistry::ModelCatalogEntry>
ProviderRegistry::GetModelCatalog() const {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  std::vector<ModelCatalogEntry> catalog;
  for (const auto& [pid, entry] : entries_) {
    for (const auto& m : entry.models) {
      ModelCatalogEntry ce;
      ce.id = m.id;
      ce.name = m.name;
      ce.provider = pid;
      ce.context_window = m.context_window;
      ce.reasoning = m.reasoning;
      ce.input = m.input;
      ce.cost = m.cost;
      ce.max_tokens = m.max_tokens;
      catalog.push_back(std::move(ce));
    }
  }
  return catalog;
}

std::string
ProviderRegistry::resolve_api_key(const ProviderEntry& entry) const {
  // Direct value
  if (!entry.api_key.empty())
    return entry.api_key;

  // Explicit env var
  if (!entry.api_key_env.empty()) {
    const char* val = std::getenv(entry.api_key_env.c_str());
    if (val)
      return val;
  }

  // Convention-based env vars
  std::string upper_id = entry.id;
  std::transform(upper_id.begin(), upper_id.end(), upper_id.begin(),
                 [](unsigned char c) {
                   if (std::isalnum(c)) {
                     return static_cast<char>(std::toupper(c));
                   }
                   return '_';
                 });

  // Try PROVIDER_API_KEY (e.g. OPENAI_API_KEY)
  std::string env_name = upper_id + "_API_KEY";
  const char* val = std::getenv(env_name.c_str());
  if (val)
    return val;

  // Try PROVIDER_KEY
  env_name = upper_id + "_KEY";
  val = std::getenv(env_name.c_str());
  if (val)
    return val;

  // Try PROVIDER_TOKEN (e.g. GITHUB_TOKEN, GITHUB_COPILOT_TOKEN)
  env_name = upper_id + "_TOKEN";
  val = std::getenv(env_name.c_str());
  if (val)
    return val;

  return "";
}

}  // namespace quantclaw
