// Copyright 2026 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0
//
// Internal support for the RPC handler registration units.
//
// register_rpc_handlers was a single 2100-line function preceded by 620 lines
// of file-static helpers. Splitting it across several implementation units
// means those helpers, and the agent-request path shared by agent.request and
// chat.send, need a shared home rather than being duplicated.
//
// Not part of the public gateway API: nothing outside the rpc_handlers
// implementation units should import this.

module;

#include <cstdlib>

#include <spdlog/spdlog.h>

export module quantclaw.gateway.rpc_helpers;

import std;
import nlohmann.json;

import quantclaw.common.atomic_file;
import quantclaw.config;
import quantclaw.constants;
import quantclaw.core.agent_loop;
import quantclaw.core.message_commands;
import quantclaw.core.prompt_builder;
import quantclaw.core.session_compaction;
import quantclaw.gateway.gateway_server;
import quantclaw.core.cron_scheduler;
import quantclaw.core.skill_loader;
import quantclaw.gateway.command_queue;
import quantclaw.plugins.plugin_system;
import quantclaw.providers.provider_registry;
import quantclaw.security.exec_approval;
import quantclaw.tools.tool_registry;
import quantclaw.gateway.protocol;
import quantclaw.providers.llm_provider;
import quantclaw.session.session_manager;

export namespace quantclaw::gateway {

// Everything the handler registration units need.
//
// register_rpc_handlers took these as fourteen positional parameters. Splitting
// registration across several units meant threading that list through each one;
// bundling it keeps the per-unit signatures to (server, deps) and makes adding
// a dependency a one-line change instead of a fourteen-site one.
struct RpcDeps {
  std::shared_ptr<quantclaw::SessionManager> session_manager;
  std::shared_ptr<quantclaw::AgentLoop> agent_loop;
  std::shared_ptr<quantclaw::PromptBuilder> prompt_builder;
  std::shared_ptr<quantclaw::ToolRegistry> tool_registry;
  const quantclaw::QuantClawConfig* config = nullptr;
  std::shared_ptr<spdlog::logger> logger;
  std::function<void()> reload_fn;
  std::shared_ptr<quantclaw::ProviderRegistry> provider_registry;
  std::shared_ptr<quantclaw::SkillLoader> skill_loader;
  std::shared_ptr<quantclaw::CronScheduler> cron_scheduler;
  std::shared_ptr<quantclaw::ExecApprovalManager> exec_approval_mgr;
  quantclaw::PluginSystem* plugin_system = nullptr;
  CommandQueue* command_queue = nullptr;
  std::string log_file_path;
};

// Registration units, called in this order by register_rpc_handlers. Each is a
// contiguous slice of the original function; order is preserved.
void RegisterCoreHandlers(GatewayServer& server, const RpcDeps& d);
void RegisterSessionHandlers(GatewayServer& server, const RpcDeps& d);
void RegisterChannelAndFileHandlers(GatewayServer& server, const RpcDeps& d);
void RegisterOpenClawCompatHandlers(GatewayServer& server, const RpcDeps& d);
void RegisterSkillAndCronHandlers(GatewayServer& server, const RpcDeps& d);
void RegisterPluginAndUsageHandlers(GatewayServer& server, const RpcDeps& d);

// Stable FNV-1a 64-bit hash of the raw config text, rendered as 16 hex chars.
// Serves as the optimistic-concurrency token for config.get/set/apply.
std::string config_hash(const std::string& raw) {
  std::uint64_t h = 1469598103934665603ULL;  // FNV-1a offset basis
  for (unsigned char c : raw) {
    h ^= c;
    h *= 1099511628211ULL;  // FNV-1a prime
  }
  static const char* kHex = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[i] = kHex[h & 0xF];
    h >>= 4;
  }
  return out;
}

// Read the on-disk config file verbatim (preserving ${VAR} placeholders and
// comments). Returns {raw, exists}.
std::pair<std::string, bool> read_config_raw() {
  auto path = QuantClawConfig::DefaultConfigPath();
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return {std::string{}, false};
  }
  std::ifstream f(path, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return {ss.str(), true};
}

// Build the ConfigSnapshot the UI expects:
// { path, exists, raw, hash, parsed, valid, config, issues }.
// The document is parsed WITHOUT ${VAR} expansion so secrets stay as
// placeholders in the editor and round-trip safely on save.
nlohmann::json build_config_snapshot() {
  auto path = QuantClawConfig::DefaultConfigPath();
  auto [raw, exists] = read_config_raw();

  nlohmann::json parsed = nlohmann::json::object();
  bool valid = true;
  nlohmann::json issues = nlohmann::json::array();

  if (exists) {
    auto p = nlohmann::json::parse(raw, nullptr, /*allow_exceptions=*/false,
                                   /*ignore_comments=*/true);
    if (p.is_discarded()) {
      valid = false;
      issues.push_back(
          {{"path", ""}, {"message", "Config file is not valid JSON"}});
    } else {
      parsed = std::move(p);
    }
  }

  return {{"path", path},   {"exists", exists}, {"raw", raw},
          {"hash", config_hash(raw)},
          {"parsed", parsed}, {"valid", valid},  {"config", parsed},
          {"issues", issues}};
}

// Validate + optimistic-concurrency check + backup + atomically replace the
// full config document. Throws std::runtime_error on invalid JSON or a hash
// conflict (file changed since the client last loaded it).
void write_config_document(const std::string& raw,
                           const std::string& base_hash) {
  auto parsed = nlohmann::json::parse(raw, nullptr, /*allow_exceptions=*/false,
                                      /*ignore_comments=*/true);
  if (parsed.is_discarded()) {
    throw std::runtime_error("config: submitted document is not valid JSON");
  }

  auto [current_raw, exists] = read_config_raw();
  if (!base_hash.empty() && config_hash(current_raw) != base_hash) {
    throw std::runtime_error(
        "config conflict: file changed since last load; reload and retry");
  }

  auto config_file = QuantClawConfig::DefaultConfigPath();
  std::error_code ec;
  if (exists) {
    std::filesystem::copy_file(
        config_file, config_file + ".bak",
        std::filesystem::copy_options::overwrite_existing, ec);
  }
  // Atomic for real, not just in the comment: a truncating ofstream leaves the
  // config empty or half-written if anything interrupts it, losing every
  // provider and key the user configured.
  std::string err;
  if (!WriteFileAtomically(config_file, raw, &err)) {
    throw std::runtime_error("config: cannot write " + config_file + ": " + err);
  }
}

// Read-modify-write a single skills.entries.<name> entry in the on-disk
// config document, backing up the previous file. Comments are not preserved
// (the document is re-serialized).
void persist_skill_entry(const std::string& skill_name,
                         const std::optional<bool>& enabled,
                         const std::optional<std::string>& api_key) {
  auto [raw, exists] = read_config_raw();
  nlohmann::json doc = nlohmann::json::object();
  if (exists) {
    auto p = nlohmann::json::parse(raw, nullptr, /*allow_exceptions=*/false,
                                   /*ignore_comments=*/true);
    if (p.is_discarded()) {
      throw std::runtime_error(
          "config: file is not valid JSON; fix it before updating skills");
    }
    doc = std::move(p);
  }

  if (!doc.is_object()) {
    doc = nlohmann::json::object();
  }
  auto& entry = doc["skills"]["entries"][skill_name];
  if (!entry.is_object()) {
    entry = nlohmann::json::object();
  }
  if (enabled.has_value()) {
    entry["enabled"] = *enabled;
  }
  if (api_key.has_value()) {
    if (api_key->empty()) {
      entry.erase("apiKey");
    } else {
      entry["apiKey"] = *api_key;
    }
  }

  auto config_file = QuantClawConfig::DefaultConfigPath();
  std::error_code ec;
  if (exists) {
    std::filesystem::copy_file(
        config_file, config_file + ".bak",
        std::filesystem::copy_options::overwrite_existing, ec);
  }
  std::string err;
  if (!WriteFileAtomically(config_file, doc.dump(2) + "\n", &err)) {
    throw std::runtime_error("config: cannot write " + config_file + ": " + err);
  }
}

// ---------------------------------------------------------------------------
// Usage / metrics engine — token accounting only (no $$ cost tracking).
// All cost fields are reported as 0 to satisfy the UI's shapes.
// ---------------------------------------------------------------------------

// Token totals in the UI's SessionsUsageTotals shape; costs always zero.
struct UsageTotals {
  long long input = 0;
  long long output = 0;
  nlohmann::json ToJson() const {
    return {{"input", input},
            {"output", output},
            {"cacheRead", 0},
            {"cacheWrite", 0},
            {"totalTokens", input + output},
            {"totalCost", 0.0},
            {"inputCost", 0.0},
            {"outputCost", 0.0},
            {"cacheReadCost", 0.0},
            {"cacheWriteCost", 0.0},
            {"missingCostEntries", 0}};
  }
};

std::string usage_date(const std::string& ts) {
  return ts.size() >= 10 ? ts.substr(0, 10) : ts;
}

// ISO-8601 "YYYY-MM-DDThh:mm:ssZ" -> epoch milliseconds (UTC) via
// days-from-civil; returns 0 if unparseable.
long long usage_iso_to_ms(const std::string& ts) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
  if (std::sscanf(ts.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) <
      3) {
    return 0;
  }
  y -= (mo <= 2);
  long long era = (y >= 0 ? y : y - 399) / 400;
  long long yoe = y - era * 400;
  long long doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long long days = era * 146097 + doe - 719468;
  return (days * 86400 + h * 3600 + mi * 60 + s) * 1000;
}

std::string usage_provider_of(const std::string& model) {
  auto slash = model.find('/');
  return slash == std::string::npos ? model : model.substr(0, slash);
}

// agent id from a session key like "agent:<id>:<...>".
std::string usage_agent_of(const std::string& key) {
  if (key.rfind("agent:", 0) == 0) {
    auto rest = key.substr(6);
    auto colon = rest.find(':');
    return colon == std::string::npos ? rest : rest.substr(0, colon);
  }
  return "main";
}

bool usage_in_range(const std::string& date, const std::string& start,
                    const std::string& end) {
  if (!start.empty() && date < start) return false;
  if (!end.empty() && date > end) return false;
  return true;
}

bool usage_is_error_result(const std::string& content) {
  return content.rfind("Error", 0) == 0 ||
         content.rfind("JSON serialization error", 0) == 0;
}

long long usage_now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Full SessionsUsageResult aggregated from every session's transcript.
// NOTE: model is not stored per-message, so token usage is attributed to the
// agent's currently-configured model for the by-model/by-provider breakdowns.
nlohmann::json compute_sessions_usage(quantclaw::SessionManager& sm,
                                      const quantclaw::QuantClawConfig& config,
                                      const std::string& start,
                                      const std::string& end) {
  const std::string model = config.agent.model;
  const std::string provider = usage_provider_of(model);

  UsageTotals totals;
  long long m_total = 0, m_user = 0, m_assistant = 0, m_toolcalls = 0,
            m_toolresults = 0, m_errors = 0;
  std::map<std::string, long long> tool_counts;

  struct Group {
    UsageTotals t;
    long long count = 0;
  };
  Group model_group, provider_group;
  std::map<std::string, Group> by_agent, by_channel;

  struct DailyAcc {
    long long input = 0, output = 0, messages = 0, toolcalls = 0, errors = 0;
  };
  std::map<std::string, DailyAcc> daily;

  nlohmann::json sessions_json = nlohmann::json::array();

  for (const auto& s : sm.ListSessions()) {
    const std::string agent = usage_agent_of(s.session_key);
    const std::string channel = s.channel.empty() ? "cli" : s.channel;

    UsageTotals s_tot;
    long long s_total = 0, s_user = 0, s_assistant = 0, s_toolcalls = 0,
              s_toolresults = 0, s_errors = 0;
    std::map<std::string, long long> s_tools;

    for (const auto& msg : sm.GetHistory(s.session_key, -1)) {
      const std::string date = usage_date(msg.timestamp);
      if (!usage_in_range(date, start, end)) continue;

      m_total++;
      s_total++;
      if (msg.role == "assistant") {
        m_assistant++;
        s_assistant++;
      } else if (msg.role == "user") {
        m_user++;
        s_user++;
      }

      for (const auto& b : msg.content) {
        if (b.type == "tool_use") {
          m_toolcalls++;
          s_toolcalls++;
          tool_counts[b.name]++;
          s_tools[b.name]++;
          daily[date].toolcalls++;
        } else if (b.type == "tool_result") {
          m_toolresults++;
          s_toolresults++;
          if (usage_is_error_result(b.content)) {
            m_errors++;
            s_errors++;
            daily[date].errors++;
          }
        }
      }
      daily[date].messages++;

      if (msg.usage) {
        long long in = msg.usage->input_tokens;
        long long out = msg.usage->output_tokens;
        totals.input += in;
        totals.output += out;
        s_tot.input += in;
        s_tot.output += out;
        model_group.t.input += in;
        model_group.t.output += out;
        model_group.count++;
        provider_group.t.input += in;
        provider_group.t.output += out;
        provider_group.count++;
        by_agent[agent].t.input += in;
        by_agent[agent].t.output += out;
        by_agent[agent].count++;
        by_channel[channel].t.input += in;
        by_channel[channel].t.output += out;
        by_channel[channel].count++;
        daily[date].input += in;
        daily[date].output += out;
      }
    }

    nlohmann::json s_tool_arr = nlohmann::json::array();
    for (const auto& [name, c] : s_tools) {
      s_tool_arr.push_back({{"name", name}, {"count", c}});
    }

    nlohmann::json usage_obj = s_tot.ToJson();
    usage_obj["messageCounts"] = {
        {"total", s_total},          {"user", s_user},
        {"assistant", s_assistant},  {"toolCalls", s_toolcalls},
        {"toolResults", s_toolresults}, {"errors", s_errors}};
    usage_obj["toolUsage"] = {{"totalCalls", s_toolcalls},
                              {"uniqueTools",
                               static_cast<long long>(s_tools.size())},
                              {"tools", s_tool_arr}};

    sessions_json.push_back({{"key", s.session_key},
                             {"label", s.display_name},
                             {"sessionId", s.session_id},
                             {"agentId", agent},
                             {"channel", channel},
                             {"model", model},
                             {"modelProvider", provider},
                             {"usage", usage_obj}});
  }

  nlohmann::json tools_arr = nlohmann::json::array();
  for (const auto& [name, c] : tool_counts) {
    tools_arr.push_back({{"name", name}, {"count", c}});
  }

  bool any_tokens = totals.input > 0 || totals.output > 0;
  nlohmann::json by_model_arr = nlohmann::json::array();
  nlohmann::json by_provider_arr = nlohmann::json::array();
  if (any_tokens) {
    by_model_arr.push_back({{"provider", provider},
                            {"model", model},
                            {"count", model_group.count},
                            {"totals", model_group.t.ToJson()}});
    by_provider_arr.push_back({{"provider", provider},
                              {"model", model},
                              {"count", provider_group.count},
                              {"totals", provider_group.t.ToJson()}});
  }

  nlohmann::json by_agent_arr = nlohmann::json::array();
  for (const auto& [a, g] : by_agent) {
    by_agent_arr.push_back({{"agentId", a}, {"totals", g.t.ToJson()}});
  }
  nlohmann::json by_channel_arr = nlohmann::json::array();
  for (const auto& [c, g] : by_channel) {
    by_channel_arr.push_back({{"channel", c}, {"totals", g.t.ToJson()}});
  }

  nlohmann::json daily_arr = nlohmann::json::array();
  for (const auto& [date, a] : daily) {
    daily_arr.push_back({{"date", date},
                         {"tokens", a.input + a.output},
                         {"cost", 0.0},
                         {"messages", a.messages},
                         {"toolCalls", a.toolcalls},
                         {"errors", a.errors}});
  }

  return {{"updatedAt", usage_now_ms()},
          {"startDate", start},
          {"endDate", end},
          {"sessions", sessions_json},
          {"totals", totals.ToJson()},
          {"aggregates",
           {{"messages",
             {{"total", m_total},
              {"user", m_user},
              {"assistant", m_assistant},
              {"toolCalls", m_toolcalls},
              {"toolResults", m_toolresults},
              {"errors", m_errors}}},
            {"tools",
             {{"totalCalls", m_toolcalls},
              {"uniqueTools", static_cast<long long>(tool_counts.size())},
              {"tools", tools_arr}}},
            {"byModel", by_model_arr},
            {"byProvider", by_provider_arr},
            {"byAgent", by_agent_arr},
            {"byChannel", by_channel_arr},
            {"daily", daily_arr}}}};
}

// CostUsageSummary shape: daily token totals + grand totals (costs zero).
nlohmann::json compute_usage_cost(quantclaw::SessionManager& sm,
                                  const std::string& start,
                                  const std::string& end) {
  std::map<std::string, UsageTotals> daily;
  UsageTotals totals;
  for (const auto& s : sm.ListSessions()) {
    for (const auto& msg : sm.GetHistory(s.session_key, -1)) {
      if (!msg.usage) continue;
      const std::string date = usage_date(msg.timestamp);
      if (!usage_in_range(date, start, end)) continue;
      daily[date].input += msg.usage->input_tokens;
      daily[date].output += msg.usage->output_tokens;
      totals.input += msg.usage->input_tokens;
      totals.output += msg.usage->output_tokens;
    }
  }
  nlohmann::json daily_arr = nlohmann::json::array();
  for (const auto& [date, t] : daily) {
    nlohmann::json e = t.ToJson();
    e["date"] = date;
    daily_arr.push_back(std::move(e));
  }
  return {{"updatedAt", usage_now_ms()},
          {"days", static_cast<long long>(daily.size())},
          {"daily", daily_arr},
          {"totals", totals.ToJson()}};
}

// Per-message token timeseries for one session (cumulative running totals).
nlohmann::json compute_session_timeseries(quantclaw::SessionManager& sm,
                                          const std::string& key) {
  nlohmann::json points = nlohmann::json::array();
  long long cum = 0;
  for (const auto& msg : sm.GetHistory(key, -1)) {
    if (!msg.usage) continue;
    long long in = msg.usage->input_tokens;
    long long out = msg.usage->output_tokens;
    if (in == 0 && out == 0) continue;
    cum += in + out;
    points.push_back({{"timestamp", usage_iso_to_ms(msg.timestamp)},
                      {"input", in},
                      {"output", out},
                      {"cacheRead", 0},
                      {"cacheWrite", 0},
                      {"totalTokens", in + out},
                      {"cost", 0.0},
                      {"cumulativeTokens", cum},
                      {"cumulativeCost", 0.0}});
  }
  return {{"sessionId", key}, {"points", points}};
}

// Per-message transcript log for one session (SessionLogEntry shape).
nlohmann::json compute_session_logs(quantclaw::SessionManager& sm,
                                    const std::string& key, long long limit) {
  nlohmann::json logs = nlohmann::json::array();
  for (const auto& msg : sm.GetHistory(key, -1)) {
    if (limit > 0 && static_cast<long long>(logs.size()) >= limit) break;

    std::string text;
    bool has_tool_result = false;
    auto append = [&](const std::string& piece) {
      if (!text.empty()) text += "\n";
      text += piece;
    };
    for (const auto& b : msg.content) {
      if (b.type == "text" || b.type == "thinking") {
        append(b.text);
      } else if (b.type == "tool_use") {
        append("\xF0\x9F\x94\xA7 " + b.name + "(" + b.input.dump() + ")");
      } else if (b.type == "tool_result") {
        has_tool_result = true;
        append(b.content);
      }
    }

    std::string role = has_tool_result
                           ? "toolResult"
                           : (msg.role == "assistant" ? "assistant" : "user");
    long long toks = msg.usage ? static_cast<long long>(
                                     msg.usage->input_tokens +
                                     msg.usage->output_tokens)
                               : 0;
    if (text.size() > 4000) text = text.substr(0, 4000) + "\xE2\x80\xA6";

    logs.push_back({{"timestamp", usage_iso_to_ms(msg.timestamp)},
                    {"role", role},
                    {"content", text},
                    {"tokens", toks},
                    {"cost", 0.0}});
  }
  return {{"logs", logs}};
}

// ---------------------------------------------------------------------------
// Agent workspace files (SOUL.md / AGENTS.md / MEMORY.md / ...): list/get/set
// for the dashboard's "edit core instructions" panel.
// ---------------------------------------------------------------------------

std::string agent_workspace_dir(const std::string& agent_id) {
  const char* home = std::getenv("HOME");
  std::string h = home ? home : "/tmp";
  std::string id = agent_id.empty() ? "main" : agent_id;
  return h + "/.quantclaw/agents/" + id + "/workspace";
}

// Core context files the editor always surfaces (missing=true if absent).
const std::vector<std::string>& agent_core_files() {
  static const std::vector<std::string> files = {
      "SOUL.md",  "IDENTITY.md", "AGENTS.md", "MEMORY.md",
      "USER.md",  "SKILL.md",    "TOOLS.md",  "HEARTBEAT.md"};
  return files;
}

// Only a bare filename is allowed (no traversal, no subdirs).
bool agent_file_name_ok(const std::string& name) {
  return !name.empty() && name.size() < 128 &&
         name.find('/') == std::string::npos &&
         name.find("..") == std::string::npos && name != ".";
}

nlohmann::json agent_file_entry(const std::string& dir, const std::string& name,
                                bool include_content) {
  std::filesystem::path p = std::filesystem::path(dir) / name;
  std::error_code ec;
  bool exists = std::filesystem::is_regular_file(p, ec);
  nlohmann::json e = {
      {"name", name}, {"path", p.string()}, {"missing", !exists}};
  if (exists) {
    e["size"] = static_cast<long long>(std::filesystem::file_size(p, ec));
    auto ftime = std::filesystem::last_write_time(p, ec);
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - std::filesystem::file_time_type::clock::now() +
        std::chrono::system_clock::now());
    e["updatedAtMs"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                           sctp.time_since_epoch())
                           .count();
    if (include_content) {
      std::ifstream f(p, std::ios::binary);
      std::ostringstream ss;
      ss << f.rdbuf();
      e["content"] = ss.str();
    }
  } else if (include_content) {
    e["content"] = "";
  }
  return e;
}

// ---------------------------------------------------------------------------
// Exec-approvals editor — dedicated ~/.quantclaw/exec-approvals.json document
// with the same {path, exists, hash, file} snapshot + optimistic-concurrency
// pattern as the main config.
// ---------------------------------------------------------------------------

std::string exec_approvals_path() {
  const char* home = std::getenv("HOME");
  std::string h = home ? home : "/tmp";
  return h + "/.quantclaw/exec-approvals.json";
}

nlohmann::json build_exec_approvals_snapshot() {
  auto path = exec_approvals_path();
  std::error_code ec;
  bool exists = std::filesystem::exists(path, ec);
  std::string raw;
  nlohmann::json file;
  if (exists) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    raw = ss.str();
    auto p = nlohmann::json::parse(raw, nullptr, /*allow_exceptions=*/false,
                                   /*ignore_comments=*/true);
    file = p.is_discarded() ? nlohmann::json::object() : p;
  } else {
    file = {{"version", 1},
            {"defaults",
             {{"ask", "on-miss"}, {"allowlist", nlohmann::json::array()}}},
            {"agents", nlohmann::json::object()}};
    raw = file.dump(2);
  }
  return {{"path", path},
          {"exists", exists},
          {"hash", config_hash(raw)},
          {"file", file}};
}

void write_exec_approvals(const nlohmann::json& file,
                          const std::string& base_hash) {
  auto path = exec_approvals_path();
  std::error_code ec;
  bool exists = std::filesystem::exists(path, ec);
  if (exists && !base_hash.empty()) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    if (config_hash(ss.str()) != base_hash) {
      throw std::runtime_error(
          "exec-approvals conflict: file changed since last load; reload and "
          "retry");
    }
  }
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(),
                                      ec);
  if (exists) {
    std::filesystem::copy_file(
        path, path + ".bak",
        std::filesystem::copy_options::overwrite_existing, ec);
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    throw std::runtime_error("exec-approvals: cannot write " + path);
  }
  out << file.dump(2) << "\n";
}

  // --- Shared agent request helper ---
  // Extracted so both agent.request and chat.send can reuse the core logic
struct AgentRequestResult {
  std::string session_key;
  std::string final_response;
  std::string error_message;
};

AgentRequestResult ExecuteAgentRequest(
    GatewayServer& server,
    const std::shared_ptr<quantclaw::SessionManager>& session_manager,
    const std::shared_ptr<quantclaw::AgentLoop>& agent_loop,
    const std::shared_ptr<quantclaw::PromptBuilder>& prompt_builder,
    const std::shared_ptr<spdlog::logger>& logger,
    const nlohmann::json& params,
    quantclaw::AgentEventCallback event_callback) {
    std::string session_key = params.value("sessionKey", "agent:main:main");
    std::string message = params.value("message", "");
    std::string error_message;

    if (message.empty()) {
      throw std::runtime_error("message is required");
    }

    // --- In-conversation slash command interception ---
    // Check if the message is a slash command (/new, /reset, /compact, etc.)
    // before forwarding to the LLM.
    {
      quantclaw::MessageCommandParser::Handlers cmd_handlers;
      cmd_handlers.reset_session = [session_manager](const std::string& key) {
        session_manager->ResetSession(key);
      };
      cmd_handlers.compact_session = [session_manager,
                                      logger](const std::string& key) {
        auto history = session_manager->GetHistory(key, -1);
        if (history.size() > 20) {
          // Simple truncation (keep last 20 messages)
          session_manager->ResetSession(key);
          int keep = std::min(20, static_cast<int>(history.size()));
          for (int i = static_cast<int>(history.size()) - keep;
               i < static_cast<int>(history.size()); ++i) {
            session_manager->AppendMessage(key, history[i]);
          }
          logger->info("Compacted session {}: kept {} of {} messages", key,
                       keep, history.size());
        }
      };
      cmd_handlers.get_status = [session_manager](const std::string& key) {
        auto history = session_manager->GetHistory(key, -1);
        return "Session: " + key +
               "\nMessages: " + std::to_string(history.size());
      };

      quantclaw::MessageCommandParser cmd_parser(std::move(cmd_handlers));
      auto cmd_result = cmd_parser.Parse(message, session_key);
      if (cmd_result.handled) {
        return {session_key, cmd_result.reply, ""};
      }
    }

    // Get or create session
    session_manager->GetOrCreate(session_key, "", "cli");

    // Auto-generate display_name from first user message
    auto sessions = session_manager->ListSessions();
    for (const auto& s : sessions) {
      if (s.session_key == session_key && s.display_name == session_key) {
        std::string truncated = message.substr(0, 50);
        session_manager->UpdateDisplayName(session_key, truncated);
        break;
      }
    }

    // Append user message
    session_manager->AppendMessage(session_key, "user", message);

    // Build system prompt
    std::string system_prompt = prompt_builder->BuildFull();

    // Load history
    auto history = session_manager->GetHistory(session_key, 50);

    // Convert SessionMessages to LLM Messages (lossless copy)
    std::vector<quantclaw::Message> llm_history;
    for (const auto& smsg : history) {
      quantclaw::Message m;
      m.role = smsg.role;
      m.content = smsg.content;
      llm_history.push_back(m);
    }

    // Remove the last message (the one we just appended) since process_message
    // adds it
    if (!llm_history.empty()) {
      llm_history.pop_back();
    }

    // Send streaming events to the client
    std::string final_response;
    auto wrapped_callback = [&event_callback, &final_response, &error_message](
                                const quantclaw::AgentEvent& event) {
      event_callback(event);
      if (event.type != events::kMessageEnd) {
        return;
      }
      if (event.data.contains("error") && event.data["error"].is_string()) {
        error_message = event.data["error"].get<std::string>();
        return;
      }
      if (event.data.contains("content") && event.data["content"].is_string()) {
        final_response = event.data["content"].get<std::string>();
      }
    };

    auto new_messages = agent_loop->ProcessMessageStream(
      message, llm_history, system_prompt, wrapped_callback, session_key);

    // Persist all new messages (assistant + tool_result) to session transcript
    for (const auto& msg : new_messages) {
      quantclaw::SessionMessage smsg;
      smsg.role = msg.role;
      smsg.content = msg.content;
      session_manager->AppendMessage(session_key, smsg);
    }

    auto dag_run_id = agent_loop->GetLatestDagRunIdForSession(session_key);
    if (!dag_run_id.empty()) {
      session_manager->AppendCustomMessage(
          session_key, "dag_run",
          nlohmann::json{{"runId", dag_run_id}, {"status", "recorded"}},
          nlohmann::json::object(), nlohmann::json::object());
    }

    if (!error_message.empty()) {
      throw std::runtime_error(error_message);
    }

    return {session_key, final_response, ""};
  }

}  // namespace quantclaw::gateway
