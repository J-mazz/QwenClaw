// Copyright 2026 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0
//
// Channels, agent listing and files, chain execution, config apply,
// presence, exec-approval resolution, and device pairing.
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

void RegisterChannelAndFileHandlers(GatewayServer& server, const RpcDeps& d) {
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

  // --- channels.list ---
  server.RegisterHandler(
      methods::kChannelsList,
      [&config, logger](const nlohmann::json& /*params*/,
                        const ClientConnection& /*client*/) -> nlohmann::json {
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
                        const ClientConnection& /*client*/) -> nlohmann::json {
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
               const ClientConnection& /*client*/) -> nlohmann::json {
        std::string id = params.value("id", "");
        logger->info("channels.logout requested for channel '{}'", id);
        return {{"ok", true}};
      });

  // --- agents.list (OpenClaw multi-agent compat stub) ---
  // QuantClaw uses a single "main" agent; return AgentsListResult shape.
  server.RegisterHandler(
      "agents.list",
      [](const nlohmann::json& /*params*/,
         const ClientConnection& /*client*/) -> nlohmann::json {
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
                              const ClientConnection& /*client*/) -> nlohmann::json {
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
                    const ClientConnection& /*client*/) -> nlohmann::json {
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
                          const ClientConnection& /*client*/) -> nlohmann::json {
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
         const ClientConnection& /*client*/) -> nlohmann::json {
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
         const ClientConnection& /*client*/) -> nlohmann::json {
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
               const ClientConnection& /*client*/) -> nlohmann::json {
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
         const ClientConnection& /*client*/) -> nlohmann::json {
        return nlohmann::json::array();
      });

  // --- last-heartbeat ---
  server.RegisterHandler(
      "last-heartbeat",
      [&server](const nlohmann::json& /*params*/,
                const ClientConnection& /*client*/) -> nlohmann::json {
        return {{"ok", true},
                {"timestamp", usage_now_ms()},
                {"uptimeSeconds", server.GetUptimeSeconds()}};
      });

  // --- exec.approvals.get / exec.approvals.node.get ---
  // Single-node: node variant returns the same gateway-level document.
  {
    auto exec_approvals_get =
        [](const nlohmann::json& /*params*/,
           const ClientConnection& /*client*/) -> nlohmann::json {
      return build_exec_approvals_snapshot();
    };
    server.RegisterHandler("exec.approvals.get", exec_approvals_get);
    server.RegisterHandler("exec.approvals.node.get", exec_approvals_get);

    auto exec_approvals_set =
        [logger](const nlohmann::json& params,
                 const ClientConnection& /*client*/) -> nlohmann::json {
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
          const ClientConnection& /*client*/) -> nlohmann::json {
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
               const ClientConnection& /*client*/) -> nlohmann::json {
        std::string req = params.value("requestId", "");
        logger->info("device.pair.approve: requestId={}", req);
        return {{"ok", true}, {"requestId", req}};
      });
  server.RegisterHandler(
      "device.pair.reject",
      [](const nlohmann::json& params,
         const ClientConnection& /*client*/) -> nlohmann::json {
        return {{"ok", true}, {"requestId", params.value("requestId", "")}};
      });
  server.RegisterHandler(
      "device.token.revoke",
      [](const nlohmann::json& params,
         const ClientConnection& /*client*/) -> nlohmann::json {
        return {{"ok", true}, {"deviceId", params.value("deviceId", "")}};
      });

  // --- update.run ---
  server.RegisterHandler(
      "update.run",
      [](const nlohmann::json& /*params*/,
         const ClientConnection& /*client*/) -> nlohmann::json {
        return {{"ok", false},
                {"restartDelayMs", 0},
                {"message",
                 "Self-update is not available for this local build; rebuild "
                 "from source."}};
      });

  // ================================================================
  // OpenClaw-compatible RPC handlers (protocol shim)
  // ================================================================
}

}  // namespace quantclaw::gateway
