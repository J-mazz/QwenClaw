// Copyright 2026 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0
//
// OpenClaw-compatible chat methods and aliases, model and tool catalogs,
// and session preview/patch/compact.
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

void RegisterOpenClawCompatHandlers(GatewayServer& server, const RpcDeps& d) {
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

  // --- chat.send (OpenClaw) ---
  // Translates QuantClaw agent events to OpenClaw format
  server.RegisterHandler(
      methods::kOcChatSend,
      [session_manager, agent_loop, prompt_builder, &server,
       logger](const nlohmann::json& params,
               const ClientConnection& client) -> nlohmann::json {
        std::string session_key = params.value("sessionKey", "agent:main:main");
        std::string idempotency_key = params.value("idempotencyKey", "");
        auto result = ExecuteAgentRequest(
            server, session_manager, agent_loop, prompt_builder, logger, params,
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
               const ClientConnection& /*client*/) -> nlohmann::json {
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
                           const ClientConnection& /*client*/) -> nlohmann::json {
        agent_loop->Stop();
        return {{"ok", true}};
      });

  // --- health (alias for gateway.health) ---
  server.RegisterHandler(
      methods::kOcHealth,
      [&server, logger](const nlohmann::json& /*params*/,
                        const ClientConnection& /*client*/) -> nlohmann::json {
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
               const ClientConnection& /*client*/) -> nlohmann::json {
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
               const ClientConnection& /*client*/) -> nlohmann::json {
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
                              const ClientConnection& /*client*/) -> nlohmann::json {
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
               const ClientConnection& /*client*/) -> nlohmann::json {
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
               const ClientConnection& /*client*/) -> nlohmann::json {
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
               const ClientConnection& /*client*/) -> nlohmann::json {
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
}

}  // namespace quantclaw::gateway
