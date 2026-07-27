// Copyright 2026 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0
//
// Gateway status, config get/set, and the agent request entry points.
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

void RegisterCoreHandlers(GatewayServer& server, const RpcDeps& d) {
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

  // --- gateway.health ---
  server.RegisterHandler(
      methods::kGatewayHealth,
      [&server, logger](const nlohmann::json& /*params*/,
                        const ClientConnection& /*client*/) -> nlohmann::json {
        return {{"status", "ok"},
                {"uptime", server.GetUptimeSeconds()},
                {"version", quantclaw::kVersion}};
      });

  // --- gateway.status ---
  server.RegisterHandler(methods::kGatewayStatus,
                         [&server, session_manager, logger](
                             const nlohmann::json& /*params*/,
                             const ClientConnection& /*client*/) -> nlohmann::json {
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
                const ClientConnection& /*client*/) -> nlohmann::json {
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
                          const ClientConnection& /*client*/) -> nlohmann::json {
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


  // --- agent.request ---
  server.RegisterHandler(
      methods::kAgentRequest,
      [session_manager, agent_loop, prompt_builder, &server,
       logger](const nlohmann::json& params,
               const ClientConnection& client) -> nlohmann::json {
        auto result = ExecuteAgentRequest(
            server, session_manager, agent_loop, prompt_builder, logger, params,
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
                           const ClientConnection& /*client*/) -> nlohmann::json {
        agent_loop->Stop();
        return {{"ok", true}};
      });
}

}  // namespace quantclaw::gateway
