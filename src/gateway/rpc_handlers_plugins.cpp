// Copyright 2026 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0
//
// Plugins, memory status, node/device inventory, log tailing, config
// schema, and usage reporting.
//
// One of six registration units of quantclaw.gateway.rpc_handlers, split from a
// single 2100-line function. Each unit is a contiguous slice of the original;
// registration order is preserved by the order register_rpc_handlers calls
// them. Handler bodies are unchanged.

module;

#include <cstdlib>

#include <spdlog/spdlog.h>

module quantclaw.gateway.rpc_helpers;

import std;
import nlohmann.json;

import quantclaw.common.atomic_file;
import quantclaw.config;
import quantclaw.constants;
import quantclaw.core.agent_loop;
import quantclaw.core.cron_scheduler;
import quantclaw.core.memory_search;
import quantclaw.core.message_commands;
import quantclaw.core.prompt_builder;
import quantclaw.core.session_compaction;
import quantclaw.core.skill_loader;
import quantclaw.gateway.command_queue;
import quantclaw.gateway.gateway_server;
import quantclaw.gateway.protocol;
import quantclaw.plugins.plugin_system;
import quantclaw.providers.llm_provider;
import quantclaw.providers.provider_registry;
import quantclaw.security.exec_approval;
import quantclaw.session.session_manager;
import quantclaw.tools.tool_chain;
import quantclaw.tools.tool_registry;

namespace quantclaw::gateway {

void RegisterPluginAndUsageHandlers(GatewayServer& server, const RpcDeps& d) {
  // Unpacked so the handler bodies below read exactly as they did when they
  // were parameters of one big function.
  const auto& session_manager = d.session_manager;
  const auto& agent_loop = d.agent_loop;
  const auto& prompt_builder = d.prompt_builder;
  const auto& tool_registry = d.tool_registry;
  const auto& config = *d.config;
  const auto& logger = d.logger;
  const auto& reload_fn = d.reload_fn;
  const auto& provider_registry = d.provider_registry;
  const auto& skill_loader = d.skill_loader;
  const auto& cron_scheduler = d.cron_scheduler;
  const auto& exec_approval_mgr = d.exec_approval_mgr;
  auto* plugin_system = d.plugin_system;
  auto* command_queue = d.command_queue;
  const auto& log_file_path = d.log_file_path;
  (void)session_manager; (void)agent_loop; (void)prompt_builder;
  (void)tool_registry; (void)config; (void)logger; (void)reload_fn;
  (void)provider_registry; (void)skill_loader; (void)cron_scheduler;
  (void)exec_approval_mgr; (void)plugin_system; (void)command_queue;
  (void)log_file_path;

  // --- Plugin methods ---
  if (plugin_system) {
    // plugins.list
    server.RegisterHandler(
        methods::kPluginsList,
        [plugin_system](const nlohmann::json& /*params*/,
                        const ClientConnection& /*client*/) -> nlohmann::json {
          return {{"plugins", plugin_system->Registry().ToJson()}};
        });

    // plugins.tools
    server.RegisterHandler(
        methods::kPluginsTools,
        [plugin_system](const nlohmann::json& /*params*/,
                        const ClientConnection& /*client*/) -> nlohmann::json {
          return {{"tools", plugin_system->GetToolSchemas()}};
        });

    // plugins.call_tool
    server.RegisterHandler(
        methods::kPluginsCallTool,
        [plugin_system](const nlohmann::json& params,
                        const ClientConnection& /*client*/) -> nlohmann::json {
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
                        const ClientConnection& /*client*/) -> nlohmann::json {
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
                        const ClientConnection& /*client*/) -> nlohmann::json {
          return {{"providers", plugin_system->ListProviders()}};
        });

    // plugins.commands
    server.RegisterHandler(
        methods::kPluginsCommands,
        [plugin_system](const nlohmann::json& params,
                        const ClientConnection& /*client*/) -> nlohmann::json {
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
                        const ClientConnection& /*client*/) -> nlohmann::json {
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
                        const ClientConnection& /*client*/) -> nlohmann::json {
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
                        const ClientConnection& /*client*/) -> nlohmann::json {
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
                        const ClientConnection& /*client*/) -> nlohmann::json {
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
                        const ClientConnection& /*client*/) -> nlohmann::json {
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
                            const ClientConnection& /*client*/) -> nlohmann::json {
          quantclaw::MemorySearch search(logger);
          search.IndexDirectory(workspace);
          return search.Stats();
        });

    // --- memory.search ---
    server.RegisterHandler(
        methods::kMemorySearch,
        [workspace, logger](const nlohmann::json& params,
                            const ClientConnection& /*client*/) -> nlohmann::json {
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
                const ClientConnection& /*client*/) -> nlohmann::json {
        return {{"agentId", "main"},
                {"name", "QuantClaw Agent"},
                {"avatar", ""},
                {"emoji", "\xF0\x9F\xA6\x9E"}};
      });

  // --- node.list ---
  // QuantClaw is a single-node deployment; return empty list.
  server.RegisterHandler("node.list",
                         [](const nlohmann::json& /*params*/,
                            const ClientConnection& /*client*/) -> nlohmann::json {
                           return nlohmann::json::array();
                         });

  // --- device.pair.list ---
  // Device pairing not implemented; return empty list so UI doesn't hang.
  server.RegisterHandler("device.pair.list",
                         [](const nlohmann::json& /*params*/,
                            const ClientConnection& /*client*/) -> nlohmann::json {
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
                              const ClientConnection& /*client*/) -> nlohmann::json {
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
         const ClientConnection& /*client*/) -> nlohmann::json {
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
                         const ClientConnection& /*client*/) -> nlohmann::json {
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
                    const ClientConnection& /*client*/) -> nlohmann::json {
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
          const ClientConnection& /*client*/) -> nlohmann::json {
        return compute_sessions_usage(*session_manager, config,
                                      params.value("startDate", ""),
                                      params.value("endDate", ""));
      });

  // --- usage.cost ---
  // Daily token totals + grand totals (token counts only; cost fields are 0).
  server.RegisterHandler(
      "usage.cost",
      [session_manager](const nlohmann::json& params,
                        const ClientConnection& /*client*/) -> nlohmann::json {
        return compute_usage_cost(*session_manager,
                                  params.value("startDate", ""),
                                  params.value("endDate", ""));
      });

  // --- sessions.usage.timeseries ---
  // Per-message token timeseries (cumulative) for one session (param: key).
  server.RegisterHandler(
      "sessions.usage.timeseries",
      [session_manager](const nlohmann::json& params,
                        const ClientConnection& /*client*/) -> nlohmann::json {
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
                        const ClientConnection& /*client*/) -> nlohmann::json {
        std::string key = params.value("key", params.value("sessionId", ""));
        if (key.empty()) {
          return {{"logs", nlohmann::json::array()}};
        }
        long long limit = params.value("limit", 1000);
        return compute_session_logs(*session_manager, key, limit);
      });
}

}  // namespace quantclaw::gateway
