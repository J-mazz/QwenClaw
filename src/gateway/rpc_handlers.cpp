// Copyright 2025 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0
//
// register_rpc_handlers: the public entry point, now a facade.
//
// This was a single 2100-line function registering ~70 handlers, preceded by
// 620 lines of file-static helpers. The handlers are unchanged; they live in
// six contiguous registration units (rpc_handlers_*.cpp) invoked here in the
// same order they were declared in, so registration order is preserved. Shared
// helpers and the agent-request path live in quantclaw.gateway.rpc_helpers.

module;

#include <spdlog/spdlog.h>

module quantclaw.gateway.rpc_handlers;

import std;
import nlohmann.json;

import quantclaw.config;
import quantclaw.core.agent_loop;
import quantclaw.core.cron_scheduler;
import quantclaw.core.prompt_builder;
import quantclaw.core.skill_loader;
import quantclaw.gateway.command_queue;
import quantclaw.gateway.gateway_server;
import quantclaw.gateway.rpc_helpers;
import quantclaw.plugins.plugin_system;
import quantclaw.providers.provider_registry;
import quantclaw.security.exec_approval;
import quantclaw.session.session_manager;
import quantclaw.tools.tool_registry;

namespace quantclaw::gateway {

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
  RpcDeps deps;
  deps.session_manager = std::move(session_manager);
  deps.agent_loop = std::move(agent_loop);
  deps.prompt_builder = std::move(prompt_builder);
  deps.tool_registry = std::move(tool_registry);
  deps.config = &config;
  deps.logger = std::move(logger);
  deps.reload_fn = std::move(reload_fn);
  deps.provider_registry = std::move(provider_registry);
  deps.skill_loader = std::move(skill_loader);
  deps.cron_scheduler = std::move(cron_scheduler);
  deps.exec_approval_mgr = std::move(exec_approval_mgr);
  deps.plugin_system = plugin_system;
  deps.command_queue = command_queue;
  deps.log_file_path = std::move(log_file_path);

  RegisterCoreHandlers(server, deps);
  RegisterSessionHandlers(server, deps);
  RegisterChannelAndFileHandlers(server, deps);
  RegisterOpenClawCompatHandlers(server, deps);
  RegisterSkillAndCronHandlers(server, deps);
  RegisterPluginAndUsageHandlers(server, deps);

  // Ask the server rather than maintaining a tally by hand. The previous count
  // was a literal (24) plus conditional increments that had to be edited
  // whenever a handler was added, and had drifted from the truth.
  deps.logger->info("Registered {} RPC handlers",
                    server.RegisteredHandlerCount());
}

}  // namespace quantclaw::gateway
