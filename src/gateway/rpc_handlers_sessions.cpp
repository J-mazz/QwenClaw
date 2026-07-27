// Copyright 2026 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0
//
// Session listing, history, deletion, and reset.
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

void RegisterSessionHandlers(GatewayServer& server, const RpcDeps& d) {
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

  // --- sessions.list ---
  server.RegisterHandler(
      methods::kSessionsList,
      [session_manager, &config,
       logger](const nlohmann::json& params,
               const ClientConnection& /*client*/) -> nlohmann::json {
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
               const ClientConnection& /*client*/) -> nlohmann::json {
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
                             const ClientConnection& /*client*/) -> nlohmann::json {
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
                             const ClientConnection& /*client*/) -> nlohmann::json {
                           std::string session_key =
                               params.value("sessionKey", "");
                           if (session_key.empty()) {
                             throw std::runtime_error("sessionKey is required");
                           }
                           session_manager->ResetSession(session_key);
                           return {{"ok", true}};
                         });
}

}  // namespace quantclaw::gateway
