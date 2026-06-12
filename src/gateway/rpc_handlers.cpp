// Copyright 2025 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0

module;

#include <cstdlib>

#include <spdlog/spdlog.h>

module quantclaw.gateway.rpc_handlers;

import std;
import nlohmann.json;

import quantclaw.config;
import quantclaw.constants;
import quantclaw.core.agent_loop;
import quantclaw.core.message_commands;
import quantclaw.gateway.command_queue;
import quantclaw.gateway.gateway_server;
import quantclaw.gateway.protocol;
import quantclaw.providers.llm_provider;
import quantclaw.plugins.plugin_system;
import quantclaw.providers.provider_registry;
import quantclaw.security.exec_approval;
import quantclaw.session.session_manager;
import quantclaw.core.cron_scheduler;
import quantclaw.core.memory_search;
import quantclaw.core.prompt_builder;
import quantclaw.core.session_compaction;
import quantclaw.core.skill_loader;
import quantclaw.tools.tool_chain;
import quantclaw.tools.tool_registry;

namespace quantclaw::gateway {

namespace {

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
  std::ofstream out(config_file, std::ios::binary | std::ios::trunc);
  if (!out) {
    throw std::runtime_error("config: cannot write " + config_file);
  }
  out << raw;
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
  } else {
    std::filesystem::create_directories(
        std::filesystem::path(config_file).parent_path(), ec);
  }
  std::ofstream out(config_file, std::ios::binary | std::ios::trunc);
  if (!out) {
    throw std::runtime_error("config: cannot write " + config_file);
  }
  out << doc.dump(2) << "\n";
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

}  // namespace

void register_rpc_handlers(
    GatewayServer& server,
    std::shared_ptr<quantclaw::SessionManager> session_manager,
    std::shared_ptr<quantclaw::AgentLoop> agent_loop,
    std::shared_ptr<quantclaw::PromptBuilder> prompt_builder,
    std::shared_ptr<quantclaw::ToolRegistry> tool_registry,
    const quantclaw::QuantClawConfig& config,
    std::shared_ptr<spdlog::logger> logger, std::function<void()> reload_fn,
    std::shared_ptr<quantclaw::ProviderRegistry> provider_registry,
    std::shared_ptr<quantclaw::SkillLoader> skill_loader,
    std::shared_ptr<quantclaw::CronScheduler> cron_scheduler,
    std::shared_ptr<quantclaw::ExecApprovalManager> exec_approval_mgr,
    quantclaw::PluginSystem* plugin_system, CommandQueue* command_queue,
    std::string log_file_path) {
  // --- gateway.health ---
  server.RegisterHandler(
      methods::kGatewayHealth,
      [&server, logger](const nlohmann::json& /*params*/,
                        ClientConnection& /*client*/) -> nlohmann::json {
        return {{"status", "ok"},
                {"uptime", server.GetUptimeSeconds()},
                {"version", quantclaw::kVersion}};
      });

  // --- gateway.status ---
  server.RegisterHandler(methods::kGatewayStatus,
                         [&server, session_manager, logger](
                             const nlohmann::json& /*params*/,
                             ClientConnection& /*client*/) -> nlohmann::json {
                           auto sessions = session_manager->ListSessions();
                           return {{"running", true},
                                   {"port", server.GetPort()},
                                   {"connections", server.GetConnectionCount()},
                                   {"uptime", server.GetUptimeSeconds()},
                                   {"sessions", sessions.size()},
                                   {"version", quantclaw::kVersion}};
                         });

  // --- config.get ---
  // No path  -> full ConfigSnapshot { path, exists, raw, hash, parsed, valid,
  //             config, issues } read from the on-disk document.
  // With path -> dot-path value lookup (CLI `config get <path>`). Well-known
  //              paths resolve from the live in-memory config; anything else
  //              falls back to the on-disk document.
  server.RegisterHandler(
      methods::kConfigGet,
      [&config](const nlohmann::json& params,
                ClientConnection& /*client*/) -> nlohmann::json {
        std::string path_param = params.value("path", "");

        if (!path_param.empty()) {
          // Live values first: these reflect the running process even when
          // the gateway was configured programmatically (tests, embedding).
          if (path_param == "gateway.port")
            return config.gateway.port;
          if (path_param == "gateway.bind")
            return config.gateway.bind;
          if (path_param == "agent.model")
            return config.agent.model;
          if (path_param == "agent.maxIterations")
            return config.agent.max_iterations;
          if (path_param == "agent.temperature")
            return config.agent.temperature;
          if (path_param == "agent.maxTokens")
            return config.agent.max_tokens;
          if (path_param == "agent.thinking")
            return config.agent.thinking;

          auto [raw, exists] = read_config_raw();
          auto doc = exists ? nlohmann::json::parse(raw, nullptr, false, true)
                            : nlohmann::json::object();
          if (doc.is_discarded()) {
            throw std::runtime_error("Config file is not valid JSON");
          }
          const nlohmann::json* cur = &doc;
          std::size_t start = 0;
          while (true) {
            auto dot = path_param.find('.', start);
            std::string key = path_param.substr(
                start,
                dot == std::string::npos ? std::string::npos : dot - start);
            if (!cur->is_object() || !cur->contains(key)) {
              throw std::runtime_error("Unknown config path: " + path_param);
            }
            cur = &(*cur)[key];
            if (dot == std::string::npos) {
              break;
            }
            start = dot + 1;
          }
          return *cur;
        }

        return build_config_snapshot();
      });

  // --- config.set ---
  // UI contract: { raw, baseHash } -> validate, optimistic-concurrency check,
  // back up, replace the whole document, hot-reload, return the new snapshot.
  // Legacy:      { path, value }   -> dot-path single-key write (CLI).
  server.RegisterHandler(
      methods::kConfigSet,
      [logger, reload_fn](const nlohmann::json& params,
                          ClientConnection& /*client*/) -> nlohmann::json {
        if (params.contains("raw")) {
          write_config_document(params.value("raw", ""),
                                params.value("baseHash", ""));
          if (reload_fn) {
            reload_fn();
          }
          logger->info("config.set: document replaced and reloaded");
          return build_config_snapshot();
        }

        std::string path = params.value("path", "");
        if (path.empty()) {
          throw std::runtime_error("path is required");
        }
        if (!params.contains("value")) {
          throw std::runtime_error("value is required");
        }

        auto config_file = QuantClawConfig::DefaultConfigPath();
        QuantClawConfig::SetValue(config_file, path, params["value"]);
        if (reload_fn) {
          reload_fn();
        }
        return {{"ok", true}, {"path", path}};
      });

  // --- Shared agent request helper ---
  // Extracted so both agent.request and chat.send can reuse the core logic
  struct AgentRequestResult {
    std::string session_key;
    std::string final_response;
    std::string error_message;
  };

  auto execute_agent_request =
      [session_manager, agent_loop, prompt_builder, logger, &server](
          const nlohmann::json& params, ClientConnection& /*client*/,
          quantclaw::AgentEventCallback event_callback) -> AgentRequestResult {
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
  };

  // --- agent.request ---
  server.RegisterHandler(
      methods::kAgentRequest,
      [execute_agent_request, &server,
       logger](const nlohmann::json& params,
               ClientConnection& client) -> nlohmann::json {
        auto result = execute_agent_request(
            params, client,
            [&server, &client, logger](const quantclaw::AgentEvent& event) {
              RpcEvent rpc_event;
              rpc_event.event = event.type;
              rpc_event.payload = event.data;
              server.SendEventTo(client.connection_id, rpc_event);
            });
        return {{"sessionKey", result.session_key},
                {"response", result.final_response}};
      });

  // --- agent.stop ---
  server.RegisterHandler(
      methods::kAgentStop,
      [agent_loop, logger](const nlohmann::json& /*params*/,
                           ClientConnection& /*client*/) -> nlohmann::json {
        agent_loop->Stop();
        return {{"ok", true}};
      });

  // --- sessions.list ---
  server.RegisterHandler(
      methods::kSessionsList,
      [session_manager, &config,
       logger](const nlohmann::json& params,
               ClientConnection& /*client*/) -> nlohmann::json {
        int limit = params.value("limit", 0);
        int offset = params.value("offset", 0);

        auto sessions = session_manager->ListSessions();
        int total = static_cast<int>(sessions.size());
        int start = std::min(offset, total);
        int end = (limit > 0) ? std::min(start + limit, total) : total;

        // Helper: Convert ISO timestamp "YYYY-MM-DDTHH:MM:SSZ" to milliseconds
        // since epoch
        auto iso_to_ms = [](const std::string& iso_str) -> int64_t {
          if (iso_str.empty())
            return 0;
          std::tm tm = {};
          std::istringstream ss(iso_str);
          ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
          if (ss.fail())
            return 0;
          tm.tm_isdst = 0;  // UTC has no DST
#ifdef _WIN32
          auto time_t_val = _mkgmtime64(&tm);
#else
          auto time_t_val = timegm(&tm);
#endif
          if (time_t_val < 0)
            return 0;
          return static_cast<int64_t>(time_t_val) * 1000;
        };

        nlohmann::json session_rows = nlohmann::json::array();
        for (int i = start; i < end; ++i) {
          const auto& s = sessions[i];
          nlohmann::json row;
          row["key"] = s.session_key;
          row["sessionId"] = s.session_id;
          row["displayName"] = s.display_name;
          row["surface"] = s.channel.empty() ? "cli" : s.channel;
          // Convert ISO timestamp to epoch ms
          row["updatedAt"] = iso_to_ms(s.updated_at);
          // Derive kind from key pattern
          if (s.session_key.find("group:") != std::string::npos) {
            row["kind"] = "group";
          } else if (s.session_key.find("global") != std::string::npos) {
            row["kind"] = "global";
          } else {
            row["kind"] = "direct";
          }
          // Parent/subagent metadata (only if set)
          if (!s.spawned_by.empty())
            row["spawnedBy"] = s.spawned_by;
          if (s.spawn_depth > 0)
            row["spawnDepth"] = s.spawn_depth;
          if (!s.subagent_role.empty())
            row["subagentRole"] = s.subagent_role;
          session_rows.push_back(row);
        }

        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();

        return {{"ts", now_ms},
                {"path", ""},
                {"count", total},
                {"defaults",
                 {{"model", config.agent.model},
                  {"contextTokens", config.agent.context_window}}},
                {"sessions", session_rows}};
      });

  // --- sessions.history ---
  server.RegisterHandler(
      methods::kSessionsHistory,
      [session_manager,
       logger](const nlohmann::json& params,
               ClientConnection& /*client*/) -> nlohmann::json {
        std::string session_key = params.value("sessionKey", "");
        int limit = params.value("limit", -1);

        if (session_key.empty()) {
          throw std::runtime_error("sessionKey is required");
        }

        auto history = session_manager->GetHistory(session_key, limit);

        nlohmann::json result = nlohmann::json::array();
        for (const auto& msg : history) {
          nlohmann::json entry;
          entry["role"] = msg.role;
          entry["timestamp"] = msg.timestamp;

          // Return full ContentBlock array
          nlohmann::json content_arr = nlohmann::json::array();
          for (const auto& block : msg.content) {
            content_arr.push_back(block.ToJson());
          }
          entry["content"] = content_arr;

          if (msg.usage) {
            entry["usage"] = msg.usage->ToJson();
          }

          result.push_back(entry);
        }
        return result;
      });

  // --- sessions.delete ---
  server.RegisterHandler(methods::kSessionsDelete,
                         [session_manager, logger](
                             const nlohmann::json& params,
                             ClientConnection& /*client*/) -> nlohmann::json {
                           // UI sends "key"; legacy clients send "sessionKey"
                           std::string session_key = params.value("key", "");
                           if (session_key.empty())
                             session_key = params.value("sessionKey", "");
                           if (session_key.empty()) {
                             throw std::runtime_error("key is required");
                           }
                           bool deleted =
                               session_manager->DeleteSession(session_key);
                           return {{"ok", true}, {"deleted", deleted}};
                         });

  // --- sessions.reset ---
  server.RegisterHandler(methods::kSessionsReset,
                         [session_manager, logger](
                             const nlohmann::json& params,
                             ClientConnection& /*client*/) -> nlohmann::json {
                           std::string session_key =
                               params.value("sessionKey", "");
                           if (session_key.empty()) {
                             throw std::runtime_error("sessionKey is required");
                           }
                           session_manager->ResetSession(session_key);
                           return {{"ok", true}};
                         });

  // --- channels.list ---
  server.RegisterHandler(
      methods::kChannelsList,
      [&config, logger](const nlohmann::json& /*params*/,
                        ClientConnection& /*client*/) -> nlohmann::json {
        nlohmann::json result = nlohmann::json::array();
        // CLI channel is always present
        result.push_back({{"id", "cli"},
                          {"type", "cli"},
                          {"enabled", true},
                          {"status", "active"}});
        // Add configured channels
        for (const auto& [id, ch] : config.channels) {
          nlohmann::json entry;
          entry["id"] = id;
          entry["type"] =
              id;  // type is typically same as id (discord, telegram, etc.)
          entry["enabled"] = ch.enabled;
          entry["status"] = ch.enabled ? "active" : "disabled";
          result.push_back(entry);
        }
        return result;
      });

  // --- channels.status ---
  // Returns ChannelsStatusSnapshot shape expected by the UI.
  server.RegisterHandler(
      methods::kChannelsStatus,
      [&config, logger](const nlohmann::json& /*params*/,
                        ClientConnection& /*client*/) -> nlohmann::json {
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();

        // Build ordered list: cli first, then configured channels
        nlohmann::json channel_order = nlohmann::json::array();
        nlohmann::json channel_labels = nlohmann::json::object();
        nlohmann::json channels = nlohmann::json::object();
        nlohmann::json channel_accounts = nlohmann::json::object();
        nlohmann::json channel_default_account = nlohmann::json::object();

        auto add_channel = [&](const std::string& cid, bool enabled,
                               const std::string& label) {
          channel_order.push_back(cid);
          channel_labels[cid] = label;
          channels[cid] = {{"enabled", enabled},
                           {"running", enabled},
                           {"configured", enabled}};
          // Each channel has a default account entry
          nlohmann::json account;
          account["accountId"] = cid + ":default";
          account["enabled"] = enabled;
          account["configured"] = enabled;
          account["running"] = enabled;
          account["connected"] = enabled;
          channel_accounts[cid] = nlohmann::json::array({account});
          channel_default_account[cid] = cid + ":default";
        };

        add_channel("cli", true, "CLI");
        for (const auto& [cid, ch] : config.channels) {
          std::string label = cid;
          label[0] = static_cast<char>(
              std::toupper(static_cast<unsigned char>(label[0])));
          add_channel(cid, ch.enabled, label);
        }

        return {{"ts", now_ms},
                {"channelOrder", channel_order},
                {"channelLabels", channel_labels},
                {"channels", channels},
                {"channelAccounts", channel_accounts},
                {"channelDefaultAccountId", channel_default_account}};
      });

  // --- channels.logout (OpenClaw compat stub) ---
  server.RegisterHandler(
      "channels.logout",
      [logger](const nlohmann::json& params,
               ClientConnection& /*client*/) -> nlohmann::json {
        std::string id = params.value("id", "");
        logger->info("channels.logout requested for channel '{}'", id);
        return {{"ok", true}};
      });

  // --- agents.list (OpenClaw multi-agent compat stub) ---
  // QuantClaw uses a single "main" agent; return AgentsListResult shape.
  server.RegisterHandler(
      "agents.list",
      [](const nlohmann::json& /*params*/,
         ClientConnection& /*client*/) -> nlohmann::json {
        return {{"defaultId", "main"},
                {"mainKey", "agent:main:main"},
                {"scope", "local"},
                {"agents", nlohmann::json::array(
                               {nlohmann::json{{"id", "main"},
                                               {"name", "QuantClaw Agent"},
                                               {"identity",
                                                {{"name", "QuantClaw Agent"},
                                                 {"theme", "default"},
                                                 {"emoji", "\xF0\x9F\xA6\x9E"},
                                                 {"avatar", ""}}}}})}};
      });

  // --- chain.execute ---
  server.RegisterHandler(
      methods::kChainExecute,
      [tool_registry, logger](const nlohmann::json& params,
                              ClientConnection& /*client*/) -> nlohmann::json {
        auto chain_def = quantclaw::ToolChainExecutor::ParseChain(params);
        quantclaw::ToolExecutorFn executor =
            [tool_registry](const std::string& name,
                            const nlohmann::json& args) {
              return tool_registry->ExecuteTool(name, args);
            };
        quantclaw::ToolChainExecutor chain_executor(executor, logger);
        auto result = chain_executor.Execute(chain_def);
        return quantclaw::ToolChainExecutor::ResultToJson(result);
      });

  // --- config.reload (pure hot-reload, no document change) ---
  if (reload_fn) {
    server.RegisterHandler(
        methods::kConfigReload,
        [reload_fn](const nlohmann::json& /*params*/,
                    ClientConnection& /*client*/) -> nlohmann::json {
          reload_fn();
          return {{"ok", true}};
        });
  }

  // --- config.apply ---
  // UI contract: { raw, baseHash, sessionKey? } -> write the document then
  // hot-reload. With no `raw` it degrades to a pure reload (OpenClaw alias).
  server.RegisterHandler(
      "config.apply",
      [reload_fn, logger](const nlohmann::json& params,
                          ClientConnection& /*client*/) -> nlohmann::json {
        if (params.contains("raw")) {
          write_config_document(params.value("raw", ""),
                                params.value("baseHash", ""));
          logger->info("config.apply: document replaced");
        }
        if (reload_fn) {
          reload_fn();
        }
        return build_config_snapshot();
      });

  // --- agents.files.list ---
  // List the agent's workspace context files (SOUL.md, AGENTS.md, ...).
  server.RegisterHandler(
      "agents.files.list",
      [](const nlohmann::json& params,
         ClientConnection& /*client*/) -> nlohmann::json {
        std::string agent_id = params.value("agentId", "main");
        std::string dir = agent_workspace_dir(agent_id);
        nlohmann::json files = nlohmann::json::array();
        std::set<std::string> seen;
        for (const auto& name : agent_core_files()) {
          files.push_back(agent_file_entry(dir, name, false));
          seen.insert(name);
        }
        // Surface any other *.md files present in the workspace too.
        std::error_code ec;
        if (std::filesystem::is_directory(dir, ec)) {
          for (const auto& de : std::filesystem::directory_iterator(dir, ec)) {
            if (!de.is_regular_file()) continue;
            auto fn = de.path().filename().string();
            if (fn.size() > 3 && fn.substr(fn.size() - 3) == ".md" &&
                seen.insert(fn).second) {
              files.push_back(agent_file_entry(dir, fn, false));
            }
          }
        }
        return {{"agentId", agent_id}, {"workspace", dir}, {"files", files}};
      });

  // --- agents.files.get ---
  server.RegisterHandler(
      "agents.files.get",
      [](const nlohmann::json& params,
         ClientConnection& /*client*/) -> nlohmann::json {
        std::string agent_id = params.value("agentId", "main");
        std::string name = params.value("name", "");
        if (!agent_file_name_ok(name)) {
          throw std::runtime_error("agents.files.get: invalid file name");
        }
        std::string dir = agent_workspace_dir(agent_id);
        return {{"agentId", agent_id},
                {"workspace", dir},
                {"file", agent_file_entry(dir, name, true)}};
      });

  // --- agents.files.set ---
  server.RegisterHandler(
      "agents.files.set",
      [logger](const nlohmann::json& params,
               ClientConnection& /*client*/) -> nlohmann::json {
        std::string agent_id = params.value("agentId", "main");
        std::string name = params.value("name", "");
        if (!agent_file_name_ok(name)) {
          throw std::runtime_error("agents.files.set: invalid file name");
        }
        std::string content = params.value("content", "");
        std::string dir = agent_workspace_dir(agent_id);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::filesystem::path p = std::filesystem::path(dir) / name;
        {
          std::ofstream out(p, std::ios::binary | std::ios::trunc);
          if (!out) {
            throw std::runtime_error("agents.files.set: cannot write " +
                                     p.string());
          }
          out << content;
        }
        logger->info("agents.files.set: wrote {} ({} bytes)", p.string(),
                     content.size());
        return {{"ok", true},
                {"agentId", agent_id},
                {"workspace", dir},
                {"file", agent_file_entry(dir, name, true)}};
      });

  // --- system-presence ---
  // Single-node local gateway: no remote presence to report (empty array).
  server.RegisterHandler(
      "system-presence",
      [](const nlohmann::json& /*params*/,
         ClientConnection& /*client*/) -> nlohmann::json {
        return nlohmann::json::array();
      });

  // --- last-heartbeat ---
  server.RegisterHandler(
      "last-heartbeat",
      [&server](const nlohmann::json& /*params*/,
                ClientConnection& /*client*/) -> nlohmann::json {
        return {{"ok", true},
                {"timestamp", usage_now_ms()},
                {"uptimeSeconds", server.GetUptimeSeconds()}};
      });

  // --- exec.approvals.get / exec.approvals.node.get ---
  // Single-node: node variant returns the same gateway-level document.
  {
    auto exec_approvals_get =
        [](const nlohmann::json& /*params*/,
           ClientConnection& /*client*/) -> nlohmann::json {
      return build_exec_approvals_snapshot();
    };
    server.RegisterHandler("exec.approvals.get", exec_approvals_get);
    server.RegisterHandler("exec.approvals.node.get", exec_approvals_get);

    auto exec_approvals_set =
        [logger](const nlohmann::json& params,
                 ClientConnection& /*client*/) -> nlohmann::json {
      write_exec_approvals(params.value("file", nlohmann::json::object()),
                           params.value("baseHash", ""));
      logger->info("exec.approvals.set: document replaced");
      return build_exec_approvals_snapshot();
    };
    server.RegisterHandler("exec.approvals.set", exec_approvals_set);
    server.RegisterHandler("exec.approvals.node.set", exec_approvals_set);
  }

  // --- exec.approval.resolve ---
  server.RegisterHandler(
      "exec.approval.resolve",
      [exec_approval_mgr, logger](
          const nlohmann::json& params,
          ClientConnection& /*client*/) -> nlohmann::json {
        std::string id = params.value("id", "");
        if (id.empty()) {
          throw std::runtime_error("exec.approval.resolve: id is required");
        }
        std::string d;
        for (char c : params.value("decision", std::string{})) {
          d += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        quantclaw::ApprovalDecision dec =
            (d.find("appr") != std::string::npos ||
             d.find("allow") != std::string::npos || d == "yes")
                ? quantclaw::ApprovalDecision::kApproved
                : quantclaw::ApprovalDecision::kDenied;
        bool ok =
            exec_approval_mgr && exec_approval_mgr->Resolve(id, dec, "operator");
        logger->info("exec.approval.resolve: id={} decision={} ok={}", id,
                     quantclaw::ApprovalDecisionToString(dec), ok);
        return {{"ok", ok},
                {"id", id},
                {"decision", quantclaw::ApprovalDecisionToString(dec)}};
      });

  // --- device.pair.approve / device.pair.reject / device.token.revoke ---
  // A single-node local gateway authenticates by shared token and keeps no
  // remote-device pairing store, so these succeed as no-ops — the devices
  // panel functions without errors. (Full device-token issuance/validation is
  // tracked separately.)
  server.RegisterHandler(
      "device.pair.approve",
      [logger](const nlohmann::json& params,
               ClientConnection& /*client*/) -> nlohmann::json {
        std::string req = params.value("requestId", "");
        logger->info("device.pair.approve: requestId={}", req);
        return {{"ok", true}, {"requestId", req}};
      });
  server.RegisterHandler(
      "device.pair.reject",
      [](const nlohmann::json& params,
         ClientConnection& /*client*/) -> nlohmann::json {
        return {{"ok", true}, {"requestId", params.value("requestId", "")}};
      });
  server.RegisterHandler(
      "device.token.revoke",
      [](const nlohmann::json& params,
         ClientConnection& /*client*/) -> nlohmann::json {
        return {{"ok", true}, {"deviceId", params.value("deviceId", "")}};
      });

  // --- update.run ---
  server.RegisterHandler(
      "update.run",
      [](const nlohmann::json& /*params*/,
         ClientConnection& /*client*/) -> nlohmann::json {
        return {{"ok", false},
                {"restartDelayMs", 0},
                {"message",
                 "Self-update is not available for this local build; rebuild "
                 "from source."}};
      });

  // ================================================================
  // OpenClaw-compatible RPC handlers (protocol shim)
  // ================================================================

  // --- chat.send (OpenClaw) ---
  // Translates QuantClaw agent events to OpenClaw format
  server.RegisterHandler(
      methods::kOcChatSend,
      [execute_agent_request, &server,
       logger](const nlohmann::json& params,
               ClientConnection& client) -> nlohmann::json {
        std::string session_key = params.value("sessionKey", "agent:main:main");
        std::string idempotency_key = params.value("idempotencyKey", "");
        auto result = execute_agent_request(
            params, client,
            [&server, &client, logger, session_key,
             idempotency_key](const quantclaw::AgentEvent& event) {
              RpcEvent rpc_event;

              if (event.type == events::kTextDelta) {
                // agent.text_delta → event "chat" {state:"delta",
                // message:{content}, runId, sessionKey}
                rpc_event.event = events::kOcChat;
                rpc_event.payload = {
                    {"state", "delta"},
                    {"message",
                     {{"role", "assistant"},
                      {"content", event.data.value("text", "")}}},
                    {"runId", idempotency_key},
                    {"sessionKey", session_key}};
              } else if (event.type == events::kToolUse) {
                // agent.tool_use → event "agent" {stream:"tool",
                // data:{id,name,input}}
                rpc_event.event = events::kOcAgent;
                rpc_event.payload = {
                    {"stream", "tool"},
                    {"data",
                     {{"id", event.data.value("id", "")},
                      {"name", event.data.value("name", "")},
                      {"input",
                       event.data.value("input", nlohmann::json::object())}}}};
              } else if (event.type == events::kToolResult) {
                // agent.tool_result → event "agent" {stream:"tool_result",
                // data:{tool_use_id,content}}
                rpc_event.event = events::kOcAgent;
                rpc_event.payload = {
                    {"stream", "tool_result"},
                    {"data",
                     {{"tool_use_id", event.data.value("tool_use_id", "")},
                      {"content", event.data.value("content", "")}}}};
              } else if (event.type == events::kMessageEnd) {
                // agent.message_end → event "chat" {state:"final", message,
                // runId, sessionKey}
                rpc_event.event = events::kOcChat;
                rpc_event.payload = {
                    {"state", "final"},
                    {"message",
                     {{"role", "assistant"},
                      {"content", event.data.value("content", "")}}},
                    {"runId", idempotency_key},
                    {"sessionKey", session_key}};
              } else {
                // Pass through any other events as-is
                rpc_event.event = event.type;
                rpc_event.payload = event.data;
              }

              server.SendEventTo(client.connection_id, rpc_event);
            });
        return {{"sessionKey", result.session_key},
                {"response", result.final_response}};
      });

  // --- chat.history (alias for sessions.history) ---
  server.RegisterHandler(
      methods::kOcChatHistory,
      [session_manager,
       logger](const nlohmann::json& params,
               ClientConnection& /*client*/) -> nlohmann::json {
        std::string session_key = params.value("sessionKey", "");
        int limit = params.value("limit", -1);

        if (session_key.empty()) {
          throw std::runtime_error("sessionKey is required");
        }

        auto history = session_manager->GetHistory(session_key, limit);

        nlohmann::json messages = nlohmann::json::array();
        for (const auto& msg : history) {
          nlohmann::json entry;
          entry["role"] = msg.role;
          entry["timestamp"] = msg.timestamp;

          nlohmann::json content_arr = nlohmann::json::array();
          for (const auto& block : msg.content) {
            content_arr.push_back(block.ToJson());
          }
          entry["content"] = content_arr;

          if (msg.usage) {
            entry["usage"] = msg.usage->ToJson();
          }

          messages.push_back(entry);
        }
        // UI expects {messages:[], thinkingLevel?:string}
        return {{"messages", messages}, {"thinkingLevel", nullptr}};
      });

  // --- chat.abort (alias for agent.stop) ---
  server.RegisterHandler(
      methods::kOcChatAbort,
      [agent_loop, logger](const nlohmann::json& /*params*/,
                           ClientConnection& /*client*/) -> nlohmann::json {
        agent_loop->Stop();
        return {{"ok", true}};
      });

  // --- health (alias for gateway.health) ---
  server.RegisterHandler(
      methods::kOcHealth,
      [&server, logger](const nlohmann::json& /*params*/,
                        ClientConnection& /*client*/) -> nlohmann::json {
        return {{"status", "ok"},
                {"uptime", server.GetUptimeSeconds()},
                {"version", quantclaw::kVersion}};
      });

  // --- status (alias for gateway.status) ---
  // Returns an OpenClaw-compatible StatusSummary so OC clients don't crash
  // on missing fields (heartbeat, sessions.byAgent, channelSummary, etc.).
  server.RegisterHandler(
      methods::kOcStatus,
      [&server, session_manager, &config,
       logger](const nlohmann::json& /*params*/,
               ClientConnection& /*client*/) -> nlohmann::json {
        auto sessions = session_manager->ListSessions();

        // Build sessions.recent (last 5, lightweight)
        nlohmann::json recent = nlohmann::json::array();
        int recent_count = std::min(5, static_cast<int>(sessions.size()));
        for (int i = static_cast<int>(sessions.size()) - recent_count;
             i < static_cast<int>(sessions.size()); ++i) {
          recent.push_back({{"key", sessions[i].session_key},
                            {"sessionId", sessions[i].session_id},
                            {"updatedAt", sessions[i].updated_at},
                            {"model", config.agent.model}});
        }

        return {// QuantClaw fields
                {"running", true},
                {"port", server.GetPort()},
                {"connections", server.GetConnectionCount()},
                {"uptime", server.GetUptimeSeconds()},
                {"version", quantclaw::kVersion},
                // OpenClaw compatibility fields
                {"heartbeat",
                 {{"defaultAgentId", "default"},
                  {"agents", nlohmann::json::array()}}},
                {"channelSummary", nlohmann::json::array()},
                {"queuedSystemEvents", nlohmann::json::array()},
                {"sessions",
                 {{"count", sessions.size()},
                  {"paths", nlohmann::json::array()},
                  {"defaults",
                   {{"model", config.agent.model},
                    {"contextTokens", config.agent.max_tokens}}},
                  {"recent", recent},
                  {"byAgent", nlohmann::json::array()}}}};
      });

  // --- models.list ---
  // Returns {models:[], current, aliases} shape expected by the UI.
  server.RegisterHandler(
      methods::kOcModelsList,
      [&config, provider_registry,
       logger](const nlohmann::json& /*params*/,
               ClientConnection& /*client*/) -> nlohmann::json {
        nlohmann::json models = nlohmann::json::array();

        // Active model first
        models.push_back({{"id", config.agent.model},
                          {"provider", "default"},
                          {"active", true}});

        // List models from registered providers
        if (provider_registry) {
          for (const auto& pid : provider_registry->ProviderIds()) {
            auto p = provider_registry->GetProvider(pid);
            if (p) {
              for (const auto& m : p->GetSupportedModels()) {
                if (m == config.agent.model)
                  continue;  // already added
                models.push_back(
                    {{"id", m}, {"provider", pid}, {"active", false}});
              }
            }
          }
        }

        return {{"models", models},
                {"current", config.agent.model},
                {"aliases", nlohmann::json::object()}};
      });

  // --- tools.catalog ---
  // Returns ToolsCatalogResult shape expected by the UI.
  server.RegisterHandler(
      methods::kOcToolsCatalog,
      [tool_registry, logger](const nlohmann::json& params,
                              ClientConnection& /*client*/) -> nlohmann::json {
        std::string agent_id = params.value("agentId", "main");
        auto schemas = tool_registry->GetToolSchemas();

        // Build tools list for the "core" group
        nlohmann::json core_tools = nlohmann::json::array();
        for (const auto& schema : schemas) {
          core_tools.push_back(
              {{"id", schema.name},
               {"label", schema.name},
               {"description", schema.description},
               {"source", "core"},
               {"optional", true},
               {"defaultProfiles", nlohmann::json::array({"full", "coding"})}});
        }

        nlohmann::json profiles = nlohmann::json::array(
            {nlohmann::json{{"id", "minimal"}, {"label", "Minimal"}},
             nlohmann::json{{"id", "coding"}, {"label", "Coding"}},
             nlohmann::json{{"id", "messaging"}, {"label", "Messaging"}},
             nlohmann::json{{"id", "full"}, {"label", "Full"}}});

        nlohmann::json groups =
            nlohmann::json::array({nlohmann::json{{"id", "core"},
                                                  {"label", "Core Tools"},
                                                  {"source", "core"},
                                                  {"tools", core_tools}}});

        return {
            {"agentId", agent_id}, {"profiles", profiles}, {"groups", groups}};
      });

  // --- sessions.preview ---
  server.RegisterHandler(
      methods::kOcSessionsPreview,
      [session_manager,
       logger](const nlohmann::json& params,
               ClientConnection& /*client*/) -> nlohmann::json {
        std::string session_key = params.value("sessionKey", "");
        if (session_key.empty()) {
          throw std::runtime_error("sessionKey is required");
        }

        auto history = session_manager->GetHistory(session_key, 1);
        if (history.empty()) {
          return nlohmann::json::object();
        }

        const auto& msg = history.back();
        nlohmann::json entry;
        entry["role"] = msg.role;
        entry["timestamp"] = msg.timestamp;

        nlohmann::json content_arr = nlohmann::json::array();
        for (const auto& block : msg.content) {
          content_arr.push_back(block.ToJson());
        }
        entry["content"] = content_arr;

        return entry;
      });

  // --- sessions.patch ---
  server.RegisterHandler(
      methods::kSessionsPatch,
      [session_manager,
       logger](const nlohmann::json& params,
               ClientConnection& /*client*/) -> nlohmann::json {
        // UI sends "key"; legacy clients send "sessionKey"
        std::string session_key = params.value("key", "");
        if (session_key.empty())
          session_key = params.value("sessionKey", "");
        if (session_key.empty()) {
          throw std::runtime_error("key is required");
        }

        // Apply displayName / label rename
        if (params.contains("displayName")) {
          session_manager->UpdateDisplayName(
              session_key, params["displayName"].get<std::string>());
        } else if (params.contains("label")) {
          if (!params["label"].is_null()) {
            session_manager->UpdateDisplayName(
                session_key, params["label"].get<std::string>());
          }
        }
        // thinkingLevel, verboseLevel, reasoningLevel are stored in session
        // metadata in a full implementation; here we acknowledge them without
        // persisting.

        return {{"ok", true},
                {"path", ""},
                {"key", session_key},
                {"entry", {{"sessionId", ""}}}};
      });

  // --- sessions.compact ---
  server.RegisterHandler(
      methods::kSessionsCompact,
      [session_manager, agent_loop,
       logger](const nlohmann::json& params,
               ClientConnection& /*client*/) -> nlohmann::json {
        std::string session_key = params.value("sessionKey", "");
        if (session_key.empty()) {
          throw std::runtime_error("sessionKey is required");
        }

        auto history = session_manager->GetHistory(session_key);

        // Convert SessionMessage list to JSON for compaction API
        std::vector<nlohmann::json> history_json;
        for (const auto& m : history) {
          history_json.push_back(m.ToJsonl());
        }

        quantclaw::SessionCompaction compaction(logger);
        quantclaw::SessionCompaction::Options opts;
        opts.max_messages = params.value("maxMessages", 100);
        opts.keep_recent = params.value("keepRecent", 20);

        if (!compaction.NeedsCompaction(history_json, opts)) {
          return {{"compacted", false}, {"reason", "below threshold"}};
        }

        auto compacted = compaction.Truncate(history_json, opts);

        return {{"compacted", true},
                {"originalCount", static_cast<int>(history.size())},
                {"newCount", static_cast<int>(compacted.size())}};
      });

  // --- skills.status ---
  if (skill_loader) {
    server.RegisterHandler(
        methods::kSkillsStatus,
        [skill_loader, &config,
         logger](const nlohmann::json& /*params*/,
                 ClientConnection& /*client*/) -> nlohmann::json {
          const char* home = std::getenv("HOME");
          std::string home_str = home ? home : "/tmp";
          auto workspace_path = std::filesystem::path(home_str) /
                                ".quantclaw/agents/main/workspace";
          std::string managed_dir =
              (std::filesystem::path(home_str) / ".quantclaw/skills").string();

          auto skills = skill_loader->LoadSkills(config.skills, workspace_path,
                                                 /*include_all=*/true);

          nlohmann::json skill_entries = nlohmann::json::array();
          for (const auto& skill : skills) {
            bool gated = !skill_loader->CheckSkillGating(skill);
            std::string skill_key =
                skill.skill_key.empty() ? skill.name : skill.skill_key;

            bool disabled = false;
            auto entry_it = config.skills.entries.find(skill.name);
            if (entry_it != config.skills.entries.end()) {
              disabled = !entry_it->second.enabled;
            }

            // Build missing bins/env/config lists
            nlohmann::json missing_bins = nlohmann::json::array();
            nlohmann::json missing_env = nlohmann::json::array();
            for (const auto& env_var : skill.required_envs) {
              if (std::getenv(env_var.c_str()) == nullptr) {
                missing_env.push_back(env_var);
              }
            }

            // Build install options from SkillInstallInfo
            nlohmann::json install_opts = nlohmann::json::array();
            for (const auto& inst : skill.installs) {
              std::string kind = inst.EffectiveMethod();
              if (kind == "node" || kind == "go" || kind == "uv" ||
                  kind == "brew") {
                nlohmann::json opt;
                opt["id"] = kind + ":" + inst.EffectiveFormula();
                opt["kind"] = kind;
                opt["label"] =
                    inst.label.empty() ? inst.EffectiveFormula() : inst.label;
                nlohmann::json bins = nlohmann::json::array();
                for (const auto& b : inst.bins)
                  bins.push_back(b);
                if (bins.empty() && !inst.EffectiveBinary().empty()) {
                  bins.push_back(inst.EffectiveBinary());
                }
                opt["bins"] = bins;
                install_opts.push_back(opt);
              }
            }

            skill_entries.push_back(
                {{"name", skill.name},
                 {"description", skill.description},
                 {"source", "bundled"},
                 {"filePath", skill.root_dir.string()},
                 {"baseDir", skill.root_dir.string()},
                 {"skillKey", skill_key},
                 {"bundled", true},
                 {"primaryEnv", skill.primary_env},
                 {"emoji", skill.emoji},
                 {"homepage", skill.homepage},
                 {"always", skill.always},
                 {"disabled", disabled},
                 {"blockedByAllowlist", false},
                 {"eligible", !gated},
                 {"requirements",
                  {{"bins",
                    [&]() {
                      nlohmann::json a = nlohmann::json::array();
                      for (const auto& b : skill.required_bins)
                        a.push_back(b);
                      return a;
                    }()},
                   {"env",
                    [&]() {
                      nlohmann::json a = nlohmann::json::array();
                      for (const auto& e : skill.required_envs)
                        a.push_back(e);
                      return a;
                    }()},
                   {"config", nlohmann::json::array()},
                   {"os",
                    [&]() {
                      nlohmann::json a = nlohmann::json::array();
                      for (const auto& o : skill.os_restrict)
                        a.push_back(o);
                      return a;
                    }()}}},
                 {"missing",
                  {{"bins", missing_bins},
                   {"env", missing_env},
                   {"config", nlohmann::json::array()},
                   {"os", nlohmann::json::array()}}},
                 {"configChecks", nlohmann::json::array()},
                 {"install", install_opts}});
          }

          return {{"workspaceDir", workspace_path.string()},
                  {"managedSkillsDir", managed_dir},
                  {"skills", skill_entries}};
        });

    // --- skills.install ---
    server.RegisterHandler(
        methods::kSkillsInstall,
        [skill_loader, logger](const nlohmann::json& params,
                               ClientConnection& /*client*/) -> nlohmann::json {
          std::string name = params.value("name", "");
          if (name.empty()) {
            throw std::runtime_error("skill name is required");
          }

          quantclaw::SkillMetadata meta;
          meta.name = name;
          meta.root_dir = params.value("rootDir", "");

          bool ok = skill_loader->InstallSkill(meta);
          return {{"ok", ok}, {"name", name}};
        });
  }

  // --- cron.list ---
  if (cron_scheduler) {
    server.RegisterHandler(
        methods::kCronList,
        [cron_scheduler](const nlohmann::json& params,
                         ClientConnection& /*client*/) -> nlohmann::json {
          int limit = params.value("limit", 0);
          int offset = params.value("offset", 0);
          auto jobs = cron_scheduler->ListJobs();
          int total = static_cast<int>(jobs.size());
          int start = std::min(offset, total);
          int end = (limit > 0) ? std::min(start + limit, total) : total;

          auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();

          auto tp_to_ms =
              [](std::chrono::system_clock::time_point tp) -> long long {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       tp.time_since_epoch())
                .count();
          };

          nlohmann::json job_list = nlohmann::json::array();
          for (int i = start; i < end; ++i) {
            const auto& job = jobs[i];
            long long last_ms = tp_to_ms(job.last_run);
            long long next_ms = tp_to_ms(job.next_run);

            nlohmann::json state = nlohmann::json::object();
            if (next_ms > 0)
              state["nextRunAtMs"] = next_ms;
            if (last_ms > 0)
              state["lastRunAtMs"] = last_ms;

            job_list.push_back(
                {{"id", job.id},
                 {"name", job.name},
                 {"description", ""},
                 {"enabled", job.enabled},
                 {"deleteAfterRun", false},
                 {"createdAtMs", now_ms},
                 {"updatedAtMs", now_ms},
                 {"schedule", {{"kind", "cron"}, {"expr", job.schedule}}},
                 {"sessionTarget", "main"},
                 {"wakeMode", "now"},
                 {"payload", {{"kind", "agentTurn"}, {"message", job.message}}},
                 {"state", state}});
          }

          bool has_more = end < total;
          return {{"jobs", job_list},
                  {"total", total},
                  {"offset", offset},
                  {"limit", limit > 0 ? limit : total},
                  {"hasMore", has_more},
                  {"nextOffset",
                   has_more ? nlohmann::json(end) : nlohmann::json(nullptr)}};
        });

    // --- cron.add ---
    // Supports both flat format (schedule string + message) and the rich UI
    // format {name, schedule:{kind,expr,everyMs,...},
    // payload:{kind,message,...}, ...}
    server.RegisterHandler(
        methods::kCronAdd,
        [cron_scheduler](const nlohmann::json& params,
                         ClientConnection& /*client*/) -> nlohmann::json {
          std::string name = params.value("name", "");
          std::string session_key =
              params.value("sessionKey", "agent:main:main");

          // Extract schedule expression (flat or nested)
          std::string schedule;
          if (params.contains("schedule") && params["schedule"].is_object()) {
            const auto& sched = params["schedule"];
            std::string kind = sched.value("kind", "cron");
            if (kind == "cron") {
              schedule = sched.value("expr", "");
            } else if (kind == "every") {
              // Convert everyMs to approximate cron expression
              long long every_ms = sched.value("everyMs", 3600000LL);
              long long every_min = std::max(1LL, every_ms / 60000LL);
              if (every_min < 60) {
                schedule = "*/" + std::to_string(every_min) + " * * * *";
              } else {
                long long every_hr = every_min / 60;
                schedule = "0 */" + std::to_string(every_hr) + " * * *";
              }
            } else if (kind == "at") {
              schedule = sched.value("at", "0 * * * *");
            }
          } else {
            schedule = params.value("schedule", "");
          }

          // Extract message (flat or nested in payload)
          std::string message;
          if (params.contains("payload") && params["payload"].is_object()) {
            message = params["payload"].value("message", "");
          } else {
            message = params.value("message", "");
          }

          if (name.empty() && !message.empty()) {
            name = message.substr(0, std::min(message.size(), (size_t)40));
          }
          if (schedule.empty() || message.empty()) {
            throw std::runtime_error("schedule and message are required");
          }

          auto id =
              cron_scheduler->AddJob(name, schedule, message, session_key);
          return {{"ok", true}, {"id", id}};
        });

    // --- cron.remove ---
    server.RegisterHandler(
        methods::kCronRemove,
        [cron_scheduler](const nlohmann::json& params,
                         ClientConnection& /*client*/) -> nlohmann::json {
          std::string id = params.value("id", "");
          if (id.empty()) {
            throw std::runtime_error("cron job id is required");
          }
          bool removed = cron_scheduler->RemoveJob(id);
          return {{"ok", removed}};
        });
  }

  // --- cron.update ---
  // Accepts both flat format and UI's {id, patch:{...}} format.
  if (cron_scheduler) {
    server.RegisterHandler(
        methods::kCronUpdate,
        [cron_scheduler,
         logger](const nlohmann::json& params,
                 ClientConnection& /*client*/) -> nlohmann::json {
          std::string id = params.value("id", "");
          if (id.empty()) {
            throw std::runtime_error("cron job id is required");
          }

          // Flatten nested patch object if present
          nlohmann::json flat = params;
          if (params.contains("patch") && params["patch"].is_object()) {
            for (const auto& item : params["patch"].items()) {
              flat[item.key()] = item.value();
            }
          }

          auto jobs = cron_scheduler->ListJobs();
          for (const auto& job : jobs) {
            if (job.id == id) {
              std::string name = flat.value("name", job.name);
              std::string schedule = job.schedule;
              std::string message = job.message;

              // Extract schedule from nested or flat
              if (flat.contains("schedule") && flat["schedule"].is_object()) {
                const auto& s = flat["schedule"];
                if (s.value("kind", "") == "cron") {
                  schedule = s.value("expr", job.schedule);
                }
              } else if (flat.contains("schedule") &&
                         flat["schedule"].is_string()) {
                schedule = flat["schedule"].get<std::string>();
              }

              // Extract message from nested payload or flat
              if (flat.contains("payload") && flat["payload"].is_object()) {
                message = flat["payload"].value("message", job.message);
              } else if (flat.contains("message")) {
                message = flat.value("message", job.message);
              }

              cron_scheduler->RemoveJob(job.id);
              auto new_id = cron_scheduler->AddJob(name, schedule, message,
                                                   job.session_key);
              return {{"ok", true}, {"id", new_id}};
            }
          }

          throw std::runtime_error("cron job not found: " + id);
        });

    // --- cron.run ---
    server.RegisterHandler(
        methods::kCronRun,
        [cron_scheduler, agent_loop, session_manager, prompt_builder,
         logger](const nlohmann::json& params,
                 ClientConnection& /*client*/) -> nlohmann::json {
          std::string id = params.value("id", "");
          if (id.empty()) {
            throw std::runtime_error("cron job id is required");
          }

          auto jobs = cron_scheduler->ListJobs();
          for (const auto& job : jobs) {
            if (job.id == id || job.id.substr(0, id.size()) == id) {
              // Execute the cron job's message as an agent request
              auto session = session_manager->GetOrCreate(job.session_key,
                                                          job.name, "cron");
              auto history_msgs = session_manager->GetHistory(job.session_key);

              std::vector<quantclaw::Message> history;
              for (const auto& m : history_msgs) {
                quantclaw::Message msg;
                msg.role = m.role;
                msg.content = m.content;
                history.push_back(msg);
              }

              auto system_prompt = prompt_builder->BuildFull(job.session_key);
              auto new_msgs = agent_loop->ProcessMessage(job.message, history,
                                                         system_prompt,
                                                         job.session_key);

              // Store messages
              for (const auto& msg : new_msgs) {
                quantclaw::SessionMessage sm;
                sm.role = msg.role;
                sm.content = msg.content;
                session_manager->AppendMessage(job.session_key, sm);
              }

              auto dag_run_id =
                  agent_loop->GetLatestDagRunIdForSession(job.session_key);
              if (!dag_run_id.empty()) {
                session_manager->AppendCustomMessage(
                    job.session_key, "dag_run",
                    nlohmann::json{{"runId", dag_run_id},
                                   {"status", "recorded"}},
                    nlohmann::json::object(), nlohmann::json::object());
              }

              nlohmann::json r;
              r["ok"] = true;
              r["jobId"] = job.id;
              r["messagesGenerated"] = static_cast<int>(new_msgs.size());
              return r;
            }
          }

          throw std::runtime_error("cron job not found: " + id);
        });

    // --- cron.runs ---
    // Returns CronRunsResult shape expected by the UI.
    server.RegisterHandler(
        methods::kCronRuns,
        [cron_scheduler,
         logger](const nlohmann::json& params,
                 ClientConnection& /*client*/) -> nlohmann::json {
          std::string filter_id = params.value("id", "");
          int limit = params.value("limit", 0);
          int offset = params.value("offset", 0);
          auto jobs = cron_scheduler->ListJobs();

          auto tp_to_ms =
              [](std::chrono::system_clock::time_point tp) -> long long {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       tp.time_since_epoch())
                .count();
          };

          // Build synthetic run log entries from last_run timestamps
          nlohmann::json all_entries = nlohmann::json::array();
          for (const auto& job : jobs) {
            if (!filter_id.empty() && job.id != filter_id)
              continue;
            long long last_ms = tp_to_ms(job.last_run);
            long long next_ms = tp_to_ms(job.next_run);
            if (last_ms > 0) {
              all_entries.push_back(
                  {{"ts", last_ms},
                   {"jobId", job.id},
                   {"jobName", job.name},
                   {"status", "ok"},
                   {"runAtMs", last_ms},
                   {"nextRunAtMs", next_ms > 0 ? nlohmann::json(next_ms)
                                               : nlohmann::json(nullptr)}});
            }
          }

          int total = static_cast<int>(all_entries.size());
          int start = std::min(offset, total);
          int end = (limit > 0) ? std::min(start + limit, total) : total;

          nlohmann::json entries = nlohmann::json::array();
          for (int i = start; i < end; ++i)
            entries.push_back(all_entries[i]);

          bool has_more = end < total;
          return {{"entries", entries},
                  {"total", total},
                  {"offset", offset},
                  {"limit", limit > 0 ? limit : total},
                  {"hasMore", has_more},
                  {"nextOffset",
                   has_more ? nlohmann::json(end) : nlohmann::json(nullptr)}};
        });
  }

  // --- exec.approval.request ---
  if (exec_approval_mgr) {
    server.RegisterHandler(methods::kExecApprovalReq,
                           [exec_approval_mgr, logger](
                               const nlohmann::json& params,
                               ClientConnection& /*client*/) -> nlohmann::json {
                             std::string command = params.value("command", "");
                             if (command.empty()) {
                               throw std::runtime_error("command is required");
                             }

                             std::string cwd = params.value("cwd", "");
                             std::string agent_id = params.value("agentId", "");
                             std::string session_key =
                                 params.value("sessionKey", "");

                             auto decision = exec_approval_mgr->RequestApproval(
                                 command, cwd, agent_id, session_key);

                             std::string decision_str;
                             switch (decision) {
                               case quantclaw::ApprovalDecision::kApproved:
                                 decision_str = "approved";
                                 break;
                               case quantclaw::ApprovalDecision::kDenied:
                                 decision_str = "denied";
                                 break;
                               case quantclaw::ApprovalDecision::kPending:
                                 decision_str = "pending";
                                 break;
                               default:
                                 decision_str = "timeout";
                                 break;
                             }

                             return {{"decision", decision_str}};
                           });

    // --- exec.approvals.get ---
    server.RegisterHandler(
        methods::kExecApprovals,
        [exec_approval_mgr,
         logger](const nlohmann::json& /*params*/,
                 ClientConnection& /*client*/) -> nlohmann::json {
          const auto& cfg = exec_approval_mgr->GetConfig();

          std::string mode_str;
          switch (cfg.ask) {
            case quantclaw::AskMode::kOff:
              mode_str = "off";
              break;
            case quantclaw::AskMode::kOnMiss:
              mode_str = "on-miss";
              break;
            case quantclaw::AskMode::kAlways:
              mode_str = "always";
              break;
          }

          nlohmann::json patterns = nlohmann::json::array();
          for (const auto& p : cfg.allowlist) {
            patterns.push_back(p);
          }

          auto pending = exec_approval_mgr->PendingRequests();
          nlohmann::json pending_json = nlohmann::json::array();
          for (const auto& req : pending) {
            pending_json.push_back({{"id", req.id},
                                    {"command", req.command},
                                    {"cwd", req.cwd},
                                    {"agentId", req.agent_id},
                                    {"sessionKey", req.session_key}});
          }

          return {{"mode", mode_str},
                  {"allowlist", patterns},
                  {"pending", pending_json}};
        });
  }

  // --- models.set ---
  server.RegisterHandler(
      methods::kModelsSet,
      [agent_loop, logger](const nlohmann::json& params,
                           ClientConnection& /*client*/) -> nlohmann::json {
        std::string model = params.value("model", "");
        if (model.empty()) {
          throw std::runtime_error("model is required");
        }
        agent_loop->SetModel(model);
        return {{"ok", true}, {"model", model}};
      });

  // --- Plugin methods ---
  if (plugin_system) {
    // plugins.list
    server.RegisterHandler(
        methods::kPluginsList,
        [plugin_system](const nlohmann::json& /*params*/,
                        ClientConnection& /*client*/) -> nlohmann::json {
          return {{"plugins", plugin_system->Registry().ToJson()}};
        });

    // plugins.tools
    server.RegisterHandler(
        methods::kPluginsTools,
        [plugin_system](const nlohmann::json& /*params*/,
                        ClientConnection& /*client*/) -> nlohmann::json {
          return {{"tools", plugin_system->GetToolSchemas()}};
        });

    // plugins.call_tool
    server.RegisterHandler(
        methods::kPluginsCallTool,
        [plugin_system](const nlohmann::json& params,
                        ClientConnection& /*client*/) -> nlohmann::json {
          std::string name = params.value("toolName", "");
          if (name.empty())
            throw std::runtime_error("toolName is required");
          auto args = params.value("args", nlohmann::json::object());
          return plugin_system->CallTool(name, args);
        });

    // plugins.services
    server.RegisterHandler(
        methods::kPluginsServices,
        [plugin_system](const nlohmann::json& params,
                        ClientConnection& /*client*/) -> nlohmann::json {
          std::string action = params.value("action", "list");
          if (action == "start") {
            return plugin_system->StartService(params.value("serviceId", ""));
          }
          if (action == "stop") {
            return plugin_system->StopService(params.value("serviceId", ""));
          }
          return {{"services", plugin_system->ListServices()}};
        });

    // plugins.providers
    server.RegisterHandler(
        methods::kPluginsProviders,
        [plugin_system](const nlohmann::json& /*params*/,
                        ClientConnection& /*client*/) -> nlohmann::json {
          return {{"providers", plugin_system->ListProviders()}};
        });

    // plugins.commands
    server.RegisterHandler(
        methods::kPluginsCommands,
        [plugin_system](const nlohmann::json& params,
                        ClientConnection& /*client*/) -> nlohmann::json {
          std::string action = params.value("action", "list");
          if (action == "execute") {
            std::string cmd = params.value("command", "");
            auto args = params.value("args", nlohmann::json::object());
            return plugin_system->ExecuteCommand(cmd, args);
          }
          return {{"commands", plugin_system->ListCommands()}};
        });

    // plugins.gateway — forward plugin-registered gateway methods
    server.RegisterHandler(
        methods::kPluginsGateway,
        [plugin_system](const nlohmann::json& params,
                        ClientConnection& /*client*/) -> nlohmann::json {
          std::string action = params.value("action", "list");
          if (action == "list") {
            return {{"methods", plugin_system->ListGatewayMethods()}};
          }
          return {{"methods", plugin_system->ListGatewayMethods()}};
        });
  }

  // ================================================================
  // Queue management RPC handlers
  // ================================================================
  if (command_queue) {
    // --- queue.status ---
    server.RegisterHandler(
        methods::kQueueStatus,
        [command_queue](const nlohmann::json& params,
                        ClientConnection& /*client*/) -> nlohmann::json {
          std::string session_key = params.value("sessionKey", "");
          if (!session_key.empty()) {
            return command_queue->SessionQueueStatus(session_key);
          }
          return command_queue->GlobalStatus();
        });

    // --- queue.configure ---
    server.RegisterHandler(
        methods::kQueueConfigure,
        [command_queue](const nlohmann::json& params,
                        ClientConnection& /*client*/) -> nlohmann::json {
          std::string session_key = params.value("sessionKey", "");
          if (session_key.empty()) {
            // Global config update
            auto new_config = QueueConfig::FromJson(params);
            command_queue->SetConfig(new_config);
            return {{"ok", true}, {"scope", "global"}};
          }
          // Per-session config
          auto mode = QueueModeFromString(params.value("mode", "collect"));
          int debounce = params.value("debounceMs", -1);
          int cap = params.value("cap", -1);
          std::string drop = params.value("drop", "");
          command_queue->ConfigureSession(session_key, mode, debounce, cap,
                                          drop);
          return {
              {"ok", true}, {"scope", "session"}, {"sessionKey", session_key}};
        });

    // --- queue.cancel ---
    server.RegisterHandler(
        methods::kQueueCancel,
        [command_queue](const nlohmann::json& params,
                        ClientConnection& /*client*/) -> nlohmann::json {
          std::string command_id = params.value("commandId", "");
          if (command_id.empty()) {
            throw std::runtime_error("commandId is required");
          }
          bool cancelled = command_queue->Cancel(command_id);
          return {{"ok", cancelled}, {"commandId", command_id}};
        });

    // --- queue.abort ---
    server.RegisterHandler(
        methods::kQueueAbort,
        [command_queue](const nlohmann::json& params,
                        ClientConnection& /*client*/) -> nlohmann::json {
          std::string session_key = params.value("sessionKey", "");
          if (session_key.empty()) {
            throw std::runtime_error("sessionKey is required");
          }
          bool aborted = command_queue->AbortSession(session_key);
          return {{"ok", aborted}, {"sessionKey", session_key}};
        });
  }

  // --- memory.status ---
  {
    const char* home = std::getenv("HOME");
    std::string home_str = home ? home : "/tmp";
    auto workspace =
        std::filesystem::path(home_str) / ".quantclaw/agents/main/workspace";

    server.RegisterHandler(
        methods::kMemoryStatus,
        [workspace, logger](const nlohmann::json& /*params*/,
                            ClientConnection& /*client*/) -> nlohmann::json {
          quantclaw::MemorySearch search(logger);
          search.IndexDirectory(workspace);
          return search.Stats();
        });

    // --- memory.search ---
    server.RegisterHandler(
        methods::kMemorySearch,
        [workspace, logger](const nlohmann::json& params,
                            ClientConnection& /*client*/) -> nlohmann::json {
          std::string query = params.value("query", "");
          int max_results = params.value("maxResults", 10);
          if (query.empty()) {
            throw std::runtime_error("query is required");
          }
          quantclaw::MemorySearch search(logger);
          search.IndexDirectory(workspace);
          auto results = search.Search(query, max_results);
          nlohmann::json arr = nlohmann::json::array();
          for (const auto& r : results) {
            arr.push_back({{"source", r.source},
                           {"content", r.content},
                           {"score", r.score},
                           {"lineNumber", r.line_number}});
          }
          return arr;
        });
  }

  // ================================================================
  // UI compatibility handlers — missing in original implementation
  // ================================================================

  // --- agent.identity.get ---
  // Called on every UI connect to show assistant name/avatar.
  server.RegisterHandler(
      "agent.identity.get",
      [&config](const nlohmann::json& /*params*/,
                ClientConnection& /*client*/) -> nlohmann::json {
        return {{"agentId", "main"},
                {"name", "QuantClaw Agent"},
                {"avatar", ""},
                {"emoji", "\xF0\x9F\xA6\x9E"}};
      });

  // --- node.list ---
  // QuantClaw is a single-node deployment; return empty list.
  server.RegisterHandler("node.list",
                         [](const nlohmann::json& /*params*/,
                            ClientConnection& /*client*/) -> nlohmann::json {
                           return nlohmann::json::array();
                         });

  // --- device.pair.list ---
  // Device pairing not implemented; return empty list so UI doesn't hang.
  server.RegisterHandler("device.pair.list",
                         [](const nlohmann::json& /*params*/,
                            ClientConnection& /*client*/) -> nlohmann::json {
                           return nlohmann::json::array();
                         });

  // --- logs.tail ---
  // Return recent lines from the gateway log file. The cursor is a line
  // offset: omit it (or pass a stale value past EOF) to get the tail of the
  // file with reset=true; pass the previously returned cursor to receive
  // only new lines. maxBytes caps the payload size per response.
  server.RegisterHandler(
      "logs.tail",
      [logger, log_file_path](const nlohmann::json& params,
                              ClientConnection& /*client*/) -> nlohmann::json {
        int req_limit = std::max(1, params.value("limit", 200));
        long long max_bytes =
            std::max(1LL, params.value("maxBytes", 512LL * 1024));
        bool has_cursor =
            params.contains("cursor") && params["cursor"].is_number();
        long long cursor = has_cursor ? params["cursor"].get<long long>() : 0;

        nlohmann::json lines = nlohmann::json::array();
        long long new_cursor = 0;
        bool truncated = false;
        bool reset = !has_cursor;

        // Normalize the path and verify its structure before opening
        // (guards against $HOME containing '..' traversal components).
        namespace fs = std::filesystem;
        fs::path safe = fs::path(log_file_path).lexically_normal();
        bool path_ok = !safe.empty() && safe.filename() == "gateway.log" &&
                       safe.parent_path().filename() == "logs" &&
                       fs::exists(safe);
        if (path_ok) {
          std::ifstream ifs(safe);
          if (ifs.is_open()) {
            std::vector<std::string> all_lines;
            std::string line;
            while (std::getline(ifs, line)) {
              all_lines.push_back(line);
            }
            long long total = static_cast<long long>(all_lines.size());

            long long start;
            if (!has_cursor || cursor < 0 || cursor > total) {
              // Initial load, or the file shrank/rotated: tail the file.
              reset = true;
              start = std::max(0LL, total - req_limit);
            } else {
              start = cursor;
            }

            long long bytes = 0;
            long long i = start;
            for (; i < total; ++i) {
              long long line_bytes =
                  static_cast<long long>(all_lines[i].size()) + 1;
              if (!lines.empty() &&
                  (bytes + line_bytes > max_bytes ||
                   static_cast<int>(lines.size()) >= req_limit)) {
                break;
              }
              lines.push_back(all_lines[i]);
              bytes += line_bytes;
            }
            new_cursor = i;
            truncated = i < total || (reset && start > 0);
          }
        }

        return {{"file", log_file_path},
                {"cursor", new_cursor},
                {"lines", lines},
                {"reset", reset},
                {"truncated", truncated}};
      });

  // --- config.schema ---
  // Return a minimal JSON schema for the config form.
  server.RegisterHandler(
      "config.schema",
      [](const nlohmann::json& /*params*/,
         ClientConnection& /*client*/) -> nlohmann::json {
        nlohmann::json schema = {
            {"type", "object"},
            {"properties",
             {{"agent",
               {{"type", "object"},
                {"properties",
                 {{"model", {{"type", "string"}}},
                  {"maxIterations",
                   {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}}},
                  {"temperature",
                   {{"type", "number"}, {"minimum", 0}, {"maximum", 2}}},
                  {"maxTokens", {{"type", "integer"}, {"minimum", 1}}},
                  {"thinking",
                   {{"type", "string"},
                    {"enum", nlohmann::json::array(
                                 {"off", "low", "medium", "high"})}}}}}}},
              {"gateway",
               {{"type", "object"},
                {"properties",
                 {{"port",
                   {{"type", "integer"}, {"minimum", 1}, {"maximum", 65535}}},
                  {"bind", {{"type", "string"}}}}}}}}}};
        nlohmann::json ui_hints = {
            {"agent.model", {{"label", "Model"}, {"group", "Agent"}}},
            {"agent.maxIterations",
             {{"label", "Max Iterations"}, {"group", "Agent"}}},
            {"agent.temperature",
             {{"label", "Temperature"},
              {"group", "Agent"},
              {"advanced", true}}},
            {"agent.thinking",
             {{"label", "Thinking Mode"}, {"group", "Agent"}}},
            {"gateway.port", {{"label", "Port"}, {"group", "Gateway"}}},
            {"gateway.bind",
             {{"label", "Bind Address"}, {"group", "Gateway"}}}};
        return {{"schema", schema},
                {"uiHints", ui_hints},
                {"version", "1"},
                {"generatedAt", ""}};
      });

  // --- cron.status ---
  if (cron_scheduler) {
    server.RegisterHandler(
        "cron.status",
        [cron_scheduler](const nlohmann::json& /*params*/,
                         ClientConnection& /*client*/) -> nlohmann::json {
          auto jobs = cron_scheduler->ListJobs();
          int enabled_count = 0;
          long long next_wake = 0;
          for (const auto& job : jobs) {
            if (job.enabled) {
              ++enabled_count;
              auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            job.next_run.time_since_epoch())
                            .count();
              if (ms > 0 && (next_wake == 0 || ms < next_wake)) {
                next_wake = ms;
              }
            }
          }
          return {{"enabled", enabled_count > 0},
                  {"jobs", static_cast<int>(jobs.size())},
                  {"nextWakeAtMs", next_wake > 0 ? nlohmann::json(next_wake)
                                                 : nlohmann::json(nullptr)}};
        });
  }

  // --- skills.update ---
  // Enable/disable a skill or save its API key. Persists to the config file
  // (skills.entries.<name>), exports the key to the skill's primaryEnv so
  // gating passes immediately, and hot-reloads the running config.
  if (skill_loader) {
    server.RegisterHandler(
        "skills.update",
        [skill_loader, &config, logger,
         reload_fn](const nlohmann::json& params,
                    ClientConnection& /*client*/) -> nlohmann::json {
          std::string skill_key = params.value("skillKey", "");
          if (skill_key.empty()) {
            throw std::runtime_error("skillKey is required");
          }

          // Resolve skillKey -> skill (entries are keyed by skill name).
          const char* home = std::getenv("HOME");
          std::string home_str = home ? home : "/tmp";
          auto workspace_path = std::filesystem::path(home_str) /
                                ".quantclaw/agents/main/workspace";
          auto skills = skill_loader->LoadSkills(config.skills, workspace_path,
                                                 /*include_all=*/true);
          const quantclaw::SkillMetadata* match = nullptr;
          for (const auto& skill : skills) {
            std::string key =
                skill.skill_key.empty() ? skill.name : skill.skill_key;
            if (key == skill_key || skill.name == skill_key) {
              match = &skill;
              break;
            }
          }
          if (!match) {
            throw std::runtime_error("Unknown skill: " + skill_key);
          }

          std::optional<bool> enabled;
          if (params.contains("enabled") && params["enabled"].is_boolean()) {
            enabled = params["enabled"].get<bool>();
          }
          std::optional<std::string> api_key;
          if (params.contains("apiKey") && params["apiKey"].is_string()) {
            api_key = params["apiKey"].get<std::string>();
          }
          if (!enabled.has_value() && !api_key.has_value()) {
            throw std::runtime_error("enabled or apiKey is required");
          }

          persist_skill_entry(match->name, enabled, api_key);

          // Export immediately so gating reflects the change without restart.
          if (api_key.has_value() && !match->primary_env.empty()) {
            if (api_key->empty()) {
              ::unsetenv(match->primary_env.c_str());
            } else {
              ::setenv(match->primary_env.c_str(), api_key->c_str(), 1);
            }
          }

          if (reload_fn) {
            reload_fn();
          }
          logger->info("skills.update: skill={} enabled={} apiKey={}",
                       match->name,
                       enabled.has_value() ? (*enabled ? "true" : "false")
                                           : "unchanged",
                       api_key.has_value() ? "updated" : "unchanged");
          return {{"ok", true}, {"skillKey", skill_key}};
        });
  }

  // --- sessions.usage ---
  // Real token aggregation across all session transcripts (messages, tools,
  // by-model/provider/agent/channel, daily). Costs are not tracked (tokens
  // only). Optional startDate/endDate (YYYY-MM-DD) filter by message date.
  server.RegisterHandler(
      "sessions.usage",
      [session_manager, &config](
          const nlohmann::json& params,
          ClientConnection& /*client*/) -> nlohmann::json {
        return compute_sessions_usage(*session_manager, config,
                                      params.value("startDate", ""),
                                      params.value("endDate", ""));
      });

  // --- usage.cost ---
  // Daily token totals + grand totals (token counts only; cost fields are 0).
  server.RegisterHandler(
      "usage.cost",
      [session_manager](const nlohmann::json& params,
                        ClientConnection& /*client*/) -> nlohmann::json {
        return compute_usage_cost(*session_manager,
                                  params.value("startDate", ""),
                                  params.value("endDate", ""));
      });

  // --- sessions.usage.timeseries ---
  // Per-message token timeseries (cumulative) for one session (param: key).
  server.RegisterHandler(
      "sessions.usage.timeseries",
      [session_manager](const nlohmann::json& params,
                        ClientConnection& /*client*/) -> nlohmann::json {
        std::string key = params.value("key", params.value("sessionId", ""));
        if (key.empty()) {
          return {{"sessionId", nullptr}, {"points", nlohmann::json::array()}};
        }
        return compute_session_timeseries(*session_manager, key);
      });

  // --- sessions.usage.logs ---
  // Per-message transcript log for one session (params: key, limit).
  server.RegisterHandler(
      "sessions.usage.logs",
      [session_manager](const nlohmann::json& params,
                        ClientConnection& /*client*/) -> nlohmann::json {
        std::string key = params.value("key", params.value("sessionId", ""));
        if (key.empty()) {
          return {{"logs", nlohmann::json::array()}};
        }
        long long limit = params.value("limit", 1000);
        return compute_session_logs(*session_manager, key, limit);
      });

  int handler_count = 24;  // base handlers (22 + 2 memory)
  if (reload_fn)
    handler_count++;
  if (skill_loader)
    handler_count += 3;  // status, install, update
  if (cron_scheduler)
    handler_count += 7;  // list, add, remove, update, run, runs, status
  if (exec_approval_mgr)
    handler_count += 2;
  if (plugin_system)
    handler_count += 7;
  if (command_queue)
    handler_count += 4;
  handler_count +=
      10;  // ui compat: identity, node.list, device.pair.list, logs.tail,
           //            config.schema, sessions.usage, usage.cost,
           //            sessions.usage.timeseries, sessions.usage.logs
  logger->info("Registered {} RPC handlers", handler_count);
}

}  // namespace quantclaw::gateway
