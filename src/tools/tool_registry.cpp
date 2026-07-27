// Copyright 2025 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0

module;

#include <httplib.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

module quantclaw.tools.tool_registry;

import std;
import nlohmann.json;

import quantclaw.common.atomic_file;
import quantclaw.common.defer;
import quantclaw.constants;
import quantclaw.core.subagent;
import quantclaw.core.cron_scheduler;
import quantclaw.core.memory_search;
import quantclaw.mcp.mcp_tool_manager;
import quantclaw.tools.tool_chain;
import quantclaw.tools.tool_helpers;
import quantclaw.platform.process;
import quantclaw.security.exec_approval;
import quantclaw.security.sandbox;
import quantclaw.security.scope_validator;
import quantclaw.security.tool_permissions;
import quantclaw.session.session_manager;

namespace fs = std::filesystem;

namespace quantclaw {

using namespace quantclaw::tools;

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

ToolRegistry::ToolRegistry(std::shared_ptr<spdlog::logger> logger)
    : logger_(logger) {
  logger_->info("ToolRegistry initialized");
}

// ---------------------------------------------------------------------------
// register_tool helper (deduplicates schema on re-register)
// ---------------------------------------------------------------------------

void ToolRegistry::register_tool(
    const std::string& name, const std::string& description,
    nlohmann::json params_schema,
    std::function<std::string(const nlohmann::json&)> handler, bool mutating) {
  std::unique_lock lock(registry_mu_);
  tools_[name] = std::move(handler);
  tool_schemas_.erase(
      std::remove_if(tool_schemas_.begin(), tool_schemas_.end(),
                     [&name](const ToolSchema& s) { return s.name == name; }),
      tool_schemas_.end());
  tool_schemas_.push_back({name, description, params_schema.dump(),
                           std::move(params_schema), mutating});
}

bool ToolRegistry::tool_is_mutating_locked(const std::string& name) const {
  for (const auto& s : tool_schemas_) {
    if (s.name == name) {
      return s.mutating;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// RegisterBuiltinTools
// ---------------------------------------------------------------------------

void ToolRegistry::RegisterBuiltinTools() {
  // ---- read ----
  register_tool(
      "read", "Read the contents of a file",
      nlohmann::json::parse(
          R"({"type":"object","properties":{"path":{"type":"string","description":"Path to the file to read"}},"required":["path"]})"),
      [this](const nlohmann::json& p) { return read_file_tool(p); });

  // ---- write ----
  register_tool(
      "write", "Write content to a file",
      nlohmann::json::parse(
          R"({"type":"object","properties":{"path":{"type":"string","description":"Path to write"},"content":{"type":"string","description":"Content to write"}},"required":["path","content"]})"),
      [this](const nlohmann::json& p) { return write_file_tool(p); },
      /*mutating=*/true);

  // ---- edit ----
  register_tool(
      "edit", "Edit a file by replacing exact text",
      nlohmann::json::parse(
          R"({"type":"object","properties":{"path":{"type":"string"},"oldText":{"type":"string","description":"Exact text to replace"},"newText":{"type":"string","description":"Replacement text"}},"required":["path","oldText","newText"]})"),
      [this](const nlohmann::json& p) { return edit_file_tool(p); },
      /*mutating=*/true);

  // ---- exec ----
  register_tool(
      "exec", "Execute a shell command and return its output",
      nlohmann::json::parse(
          R"JSON({"type":"object","properties":{"command":{"type":"string","description":"Shell command to execute"},"workdir":{"type":"string","description":"Working directory (optional)"},"timeout":{"type":"integer","description":"Timeout in seconds (default 30)"}},"required":["command"]})JSON"),
      [this](const nlohmann::json& p) { return exec_tool(p); },
      /*mutating=*/true);

  // ---- bash (OpenClaw alias for exec) ----
  register_tool(
      "bash", "Execute a shell command (alias for exec)",
      nlohmann::json::parse(
          R"JSON({"type":"object","properties":{"command":{"type":"string","description":"Shell command to execute"},"timeout":{"type":"integer","description":"Timeout in seconds (default 30)"}},"required":["command"]})JSON"),
      [this](const nlohmann::json& p) { return exec_tool(p); },
      /*mutating=*/true);

  // ---- apply_patch ----
  register_tool(
      "apply_patch",
      "Apply a multi-file patch in *** Begin Patch / *** End Patch format. "
      "Supports: *** Add File, *** Update File (with unified diff hunks), *** "
      "Delete File.",
      nlohmann::json::parse(
          R"({"type":"object","properties":{"patch":{"type":"string","description":"Patch text in *** Begin Patch ... *** End Patch format"}},"required":["patch"]})"),
      [this](const nlohmann::json& p) { return apply_patch_tool(p); },
      /*mutating=*/true);

  // ---- process ----
  register_tool(
      "process",
      "Manage long-running background shell sessions. "
      "Actions: start, list, log, poll, write, send-keys, kill, clear, remove.",
      nlohmann::json::parse(
          R"JSON({"type":"object","properties":{"action":{"type":"string","enum":["start","list","log","poll","write","send-keys","kill","clear","remove"],"description":"Action to perform"},"command":{"type":"string","description":"Shell command (required for start)"},"id":{"type":"string","description":"Session ID (required for all actions except start/list)"},"input":{"type":"string","description":"Text to write to stdin (for write/send-keys)"},"timeout":{"type":"integer","description":"Max wait ms for poll (default 5000)"}},"required":["action"]})JSON"),
      [this](const nlohmann::json& p) { return process_tool(p); });

  // ---- message ----
  register_tool(
      "message", "Send a message to a channel",
      nlohmann::json::parse(
          R"({"type":"object","properties":{"channel":{"type":"string","description":"Channel to send to"},"message":{"type":"string","description":"Message content"},"action":{"type":"string","description":"Action: send (default), reply, react, pin, edit, delete"}},"required":["channel","message"]})"),
      [this](const nlohmann::json& p) { return message_tool(p); });

  // ---- web_search ----
  register_tool(
      "web_search",
      "Search the web. Cascade: Brave (BRAVE_API_KEY), Tavily "
      "(TAVILY_API_KEY), "
      "Perplexity (PERPLEXITY_API_KEY), DuckDuckGo (free, no key), Grok "
      "(XAI_API_KEY). "
      "Returns titles, URLs, and snippets.",
      nlohmann::json::parse(
          R"JSON({"type":"object","properties":{"query":{"type":"string","description":"Search query"},"count":{"type":"integer","description":"Number of results (1-10, default 5)"},"freshness":{"type":"string","description":"Time filter: day, week, month, year"}},"required":["query"]})JSON"),
      [this](const nlohmann::json& p) { return web_search_tool(p); });

  // ---- web_fetch ----
  register_tool(
      "web_fetch",
      "Fetch a URL and return its content as plain text or markdown.",
      nlohmann::json::parse(
          R"JSON({"type":"object","properties":{"url":{"type":"string","description":"URL to fetch"},"maxChars":{"type":"integer","description":"Max characters to return (default 50000)"}},"required":["url"]})JSON"),
      [this](const nlohmann::json& p) { return web_fetch_tool(p); });

  // ---- memory_search ----
  register_tool(
      "memory_search",
      "Search agent memory files (MEMORY.md and workspace docs) using BM25 "
      "full-text search.",
      nlohmann::json::parse(
          R"JSON({"type":"object","properties":{"query":{"type":"string","description":"Search query"},"maxResults":{"type":"integer","description":"Max results to return (default 10)"}},"required":["query"]})JSON"),
      [this](const nlohmann::json& p) { return memory_search_tool(p); });

  // ---- memory_get ----
  register_tool(
      "memory_get",
      "Read a specific file from the agent workspace (MEMORY.md, notes, etc.).",
      nlohmann::json::parse(
          R"({"type":"object","properties":{"path":{"type":"string","description":"Relative path within the workspace, e.g. MEMORY.md or memory/notes.md"}},"required":["path"]})"),
      [this](const nlohmann::json& p) { return memory_get_tool(p); });

  // ---- claude_advisor ----
  register_tool(
      "claude_advisor",
      "Consult Claude Opus 4.8 for expert software-engineering advice via the "
      "official Claude Code CLI (uses its own login; this tool never handles "
      "credentials). Read-only — returns written analysis, does not modify "
      "files. Put any relevant code/errors in 'context'. Use for hard bugs, "
      "design/architecture decisions, and reviews beyond the local model.",
      nlohmann::json::parse(
          R"JSON({"type":"object","properties":{"question":{"type":"string","description":"The question or problem to ask Claude"},"context":{"type":"string","description":"Optional supporting context: code, error logs, file excerpts"},"model":{"type":"string","enum":["opus","sonnet","haiku"],"description":"Advisor model (default opus = Opus 4.8)"},"timeout":{"type":"integer","description":"Max seconds to wait (default 300)"}},"required":["question"]})JSON"),
      [this](const nlohmann::json& p) { return claude_advisor_tool(p); });

  logger_->info("Registered {} built-in tools", tools_.size());
}

// ---------------------------------------------------------------------------
// RegisterChainTool
// ---------------------------------------------------------------------------

void ToolRegistry::RegisterChainTool() {
  auto chain_handler = [this](const nlohmann::json& params) -> std::string {
    auto chain_def = ToolChainExecutor::ParseChain(params);
    ToolExecutorFn executor = [this](const std::string& name,
                                     const nlohmann::json& args) {
      // Re-enters ExecuteTool once per step — see the registry_mu_ note in the
      // header for why no lock may be held across a handler call.
      return ExecuteTool(name, args);
    };
    ToolChainExecutor chain_executor(executor, logger_);
    auto result = chain_executor.Execute(chain_def);
    return ToolChainExecutor::ResultToJson(result).dump();
  };

  nlohmann::json chain_params;
  chain_params["type"] = "object";
  chain_params["properties"] = {
      {"name", {{"type", "string"}, {"description", "Name of the chain"}}},
      {"steps",
       {{"type", "array"},
        {"items",
         {{"type", "object"},
          {"properties",
           {{"tool", {{"type", "string"}, {"description", "Tool name"}}},
            {"arguments",
             {{"type", "object"},
              {"description",
               "Args, may use {{prev.result}} or {{steps[N].result}}"}}}}},
          {"required", {"tool"}}}},
        {"description", "Ordered tool invocations"}}},
      {"error_policy",
       {{"type", "string"},
        {"enum", {"stop_on_error", "continue_on_error", "retry"}}}},
      {"max_retries", {{"type", "integer"}}}};
  chain_params["required"] = {"steps"};

  // Route through register_tool rather than touching tools_/tool_schemas_
  // directly: it is the single place that takes registry_mu_ for a write, and
  // it already handles de-duplicating the schema on re-registration.
  register_tool("chain",
                "Execute a pipeline of tools in sequence. Each step "
                "can reference previous results via "
                "{{prev.result}} or {{steps[N].result}} templates.",
                std::move(chain_params), std::move(chain_handler));

  logger_->info("Registered chain tool");
}

// ---------------------------------------------------------------------------
// SetPermissionChecker / SetMcpToolManager / SetApprovalManager
// ---------------------------------------------------------------------------

void ToolRegistry::SetPermissionChecker(
    std::shared_ptr<ToolPermissionChecker> checker) {
  permission_checker_ = std::move(checker);
}

void ToolRegistry::SetMcpToolManager(
    std::shared_ptr<mcp::MCPToolManager> manager) {
  mcp_tool_manager_ = std::move(manager);
}

void ToolRegistry::SetApprovalManager(
    std::shared_ptr<ExecApprovalManager> manager) {
  approval_manager_ = std::move(manager);
}

// ---------------------------------------------------------------------------
// SetSubagentManager — registers spawn_subagent tool
// ---------------------------------------------------------------------------

void ToolRegistry::SetSubagentManager(SubagentManager* manager,
                                      const std::string& session_key) {
  subagent_manager_ = manager;
  current_session_key_ = session_key;
  if (!manager)
    return;

  tools_["spawn_subagent"] =
      [this](const nlohmann::json& params) -> std::string {
    if (!subagent_manager_)
      throw std::runtime_error("Subagent manager not configured");

    SpawnParams sp;
    sp.task = params.value("task", "");
    if (sp.task.empty())
      throw std::runtime_error("Missing required parameter: task");
    sp.label = params.value("label", "");
    sp.agent_id = params.value("agent_id", "");
    sp.model = params.value("model", "");
    sp.thinking = params.value("thinking", "off");
    sp.timeout_seconds = params.value("timeout", 300);
    sp.mode = spawn_mode_from_string(params.value("mode", "run"));
    sp.cleanup = params.value("cleanup", true);

    auto result = subagent_manager_->Spawn(sp, current_session_key_);

    nlohmann::json r;
    r["status"] =
      (result.status == SpawnResult::Status::kAccepted)     ? "accepted"
      : (result.status == SpawnResult::Status::kForbidden) ? "forbidden"
                                   : "error";
    if (!result.child_session_key.empty())
      r["child_session_key"] = result.child_session_key;
    if (!result.run_id.empty())
      r["run_id"] = result.run_id;
    r["mode"] = spawn_mode_to_string(result.mode);
    if (!result.note.empty())
      r["note"] = result.note;
    if (!result.error.empty())
      r["error"] = result.error;
    return r.dump();
  };

  nlohmann::json sp;
  sp["type"] = "object";
  sp["properties"] = {
      {"task", {{"type", "string"}, {"description", "Task for the subagent"}}},
      {"label", {{"type", "string"}, {"description", "Human-readable label"}}},
      {"agent_id", {{"type", "string"}, {"description", "Target agent ID"}}},
      {"model", {{"type", "string"}, {"description", "Model override"}}},
      {"thinking",
       {{"type", "string"},
        {"description", "Thinking level: off|low|medium|high"}}},
      {"timeout", {{"type", "integer"}, {"description", "Timeout in seconds"}}},
      {"mode", {{"type", "string"}, {"enum", {"run", "session"}}}},
      {"cleanup",
       {{"type", "boolean"}, {"description", "Auto-delete on completion"}}}};
  sp["required"] = {"task"};

  tool_schemas_.erase(std::remove_if(tool_schemas_.begin(), tool_schemas_.end(),
                                     [](const ToolSchema& s) {
                                       return s.name == "spawn_subagent";
                                     }),
                      tool_schemas_.end());
  tool_schemas_.push_back({"spawn_subagent",
                           "Spawn a subagent to handle a subtask autonomously.",
                           sp.dump()});
  logger_->info("Subagent manager set, spawn_subagent tool registered");
}

// ---------------------------------------------------------------------------
// SetCronScheduler — registers cron agent tool
// ---------------------------------------------------------------------------

void ToolRegistry::SetCronScheduler(std::shared_ptr<CronScheduler> sched) {
  cron_scheduler_ = std::move(sched);
  if (!cron_scheduler_)
    return;

  tools_["cron"] = [this](const nlohmann::json& params) -> std::string {
    if (!cron_scheduler_)
      throw std::runtime_error("Cron scheduler not available");
    std::string action = params.value("action", "list");

    if (action == "list" || action == "status") {
      auto jobs = cron_scheduler_->ListJobs();
      nlohmann::json result = nlohmann::json::array();
      for (const auto& job : jobs) {
        nlohmann::json entry;
        entry["id"] = job.id;
        entry["name"] = job.name;
        entry["schedule"] = job.schedule;
        entry["message"] = job.message;
        entry["enabled"] = job.enabled;
        result.push_back(entry);
      }
      return nlohmann::json{{"jobs", result}, {"count", result.size()}}.dump();
    }

    if (action == "add") {
      std::string name = params.value("name", "");
      std::string schedule = params.value("schedule", "");
      std::string message = params.value("message", "");
      std::string sess_key = params.value("sessionKey", "agent:main:main");
      if (schedule.empty() || message.empty())
        throw std::runtime_error(
            "schedule and message are required for cron add");
      if (name.empty())
        name = message.substr(0, 40);
      auto id = cron_scheduler_->AddJob(name, schedule, message, sess_key);
      return nlohmann::json{{"ok", true}, {"id", id}}.dump();
    }

    if (action == "remove") {
      std::string id = params.value("id", "");
      if (id.empty())
        throw std::runtime_error("id is required for cron remove");
      bool ok = cron_scheduler_->RemoveJob(id);
      return nlohmann::json{{"ok", ok}}.dump();
    }

    if (action == "run") {
      std::string id = params.value("id", "");
      if (id.empty())
        throw std::runtime_error("id is required for cron run");
      // Trigger is handled externally; report intent only
      return nlohmann::json{{"ok", true}, {"triggered", id}}.dump();
    }

    throw std::runtime_error("Unknown cron action: " + action);
  };

  nlohmann::json cron_params;
  cron_params["type"] = "object";
  cron_params["properties"] = {
      {"action",
       {{"type", "string"},
        {"enum", {"list", "status", "add", "remove", "run"}},
        {"description", "Action"}}},
      {"id", {{"type", "string"}, {"description", "Job ID (for remove/run)"}}},
      {"name", {{"type", "string"}, {"description", "Job name"}}},
      {"schedule",
       {{"type", "string"},
        {"description", "Cron expression (e.g. '*/5 * * * *')"}}},
      {"message",
       {{"type", "string"}, {"description", "Message to send to agent"}}},
      {"sessionKey",
       {{"type", "string"}, {"description", "Target session key"}}}};
  cron_params["required"] = {"action"};

  tool_schemas_.erase(
      std::remove_if(tool_schemas_.begin(), tool_schemas_.end(),
                     [](const ToolSchema& s) { return s.name == "cron"; }),
      tool_schemas_.end());
  tool_schemas_.push_back({"cron",
                           "Manage gateway cron jobs: list, add, remove, run.",
                           cron_params.dump()});

  logger_->info("Cron scheduler set, cron tool registered");
}

// ---------------------------------------------------------------------------
// SetSessionManager — registers sessions_list / sessions_history /
// sessions_send
// ---------------------------------------------------------------------------

void ToolRegistry::SetSessionManager(std::shared_ptr<SessionManager> mgr) {
  session_manager_ = std::move(mgr);
  if (!session_manager_)
    return;

  // sessions_list
  tools_["sessions_list"] =
      [this](const nlohmann::json& params) -> std::string {
    if (!session_manager_)
      throw std::runtime_error("Session manager not available");
    int limit = params.value("limit", 20);
    int offset = params.value("offset", 0);
    auto sessions = session_manager_->ListSessions();
    int total = static_cast<int>(sessions.size());
    int start = std::min(offset, total);
    int end = std::min(start + limit, total);
    nlohmann::json rows = nlohmann::json::array();
    for (int i = start; i < end; ++i) {
      nlohmann::json row;
      row["key"] = sessions[i].session_key;
      row["sessionId"] = sessions[i].session_id;
      row["displayName"] = sessions[i].display_name;
      row["surface"] =
          sessions[i].channel.empty() ? "cli" : sessions[i].channel;
      rows.push_back(row);
    }
    return nlohmann::json{{"sessions", rows}, {"total", total}}.dump();
  };
  tool_schemas_.erase(std::remove_if(tool_schemas_.begin(), tool_schemas_.end(),
                                     [](const ToolSchema& s) {
                                       return s.name == "sessions_list";
                                     }),
                      tool_schemas_.end());
  tool_schemas_.push_back(
      {"sessions_list", "List agent sessions.",
       nlohmann::json::parse(
         R"JSON({"type":"object","properties":{"limit":{"type":"integer","description":"Max results (default 20)"},"offset":{"type":"integer","description":"Offset for pagination"}}})JSON")
         .dump()});

  // sessions_history
  tools_["sessions_history"] =
      [this](const nlohmann::json& params) -> std::string {
    if (!session_manager_)
      throw std::runtime_error("Session manager not available");
    std::string key = params.value("sessionKey", "");
    if (key.empty())
      throw std::runtime_error("sessionKey is required");
    int limit = params.value("limit", 50);
    auto history = session_manager_->GetHistory(key, limit);
    nlohmann::json messages = nlohmann::json::array();
    for (const auto& msg : history) {
      nlohmann::json entry;
      entry["role"] = msg.role;
      entry["timestamp"] = msg.timestamp;
      nlohmann::json content = nlohmann::json::array();
      for (const auto& block : msg.content)
        content.push_back(block.ToJson());
      entry["content"] = content;
      messages.push_back(entry);
    }
    return nlohmann::json{{"messages", messages}, {"sessionKey", key}}.dump();
  };
  tool_schemas_.erase(std::remove_if(tool_schemas_.begin(), tool_schemas_.end(),
                                     [](const ToolSchema& s) {
                                       return s.name == "sessions_history";
                                     }),
                      tool_schemas_.end());
  tool_schemas_.push_back(
      {"sessions_history", "Read the transcript of a session.",
       nlohmann::json::parse(
         R"JSON({"type":"object","properties":{"sessionKey":{"type":"string","description":"Session key"},"limit":{"type":"integer","description":"Max messages (default 50)"}},"required":["sessionKey"]})JSON")
         .dump()});

  // sessions_send
  tools_["sessions_send"] =
      [this](const nlohmann::json& params) -> std::string {
    if (!session_manager_)
      throw std::runtime_error("Session manager not available");
    std::string key = params.value("sessionKey", "");
    std::string message = params.value("message", "");
    if (key.empty() || message.empty())
      throw std::runtime_error("sessionKey and message are required");
    session_manager_->AppendMessage(key, "user", message);
    return nlohmann::json{{"ok", true}, {"sessionKey", key}}.dump();
  };
  tool_schemas_.erase(std::remove_if(tool_schemas_.begin(), tool_schemas_.end(),
                                     [](const ToolSchema& s) {
                                       return s.name == "sessions_send";
                                     }),
                      tool_schemas_.end());
  tool_schemas_.push_back(
      {"sessions_send", "Send a message into another session (agent-to-agent).",
       nlohmann::json::parse(
         R"({"type":"object","properties":{"sessionKey":{"type":"string","description":"Target session key"},"message":{"type":"string","description":"Message text"}},"required":["sessionKey","message"]})")
         .dump()});

  logger_->info(
      "Session manager set: sessions_list/history/send tools registered");
}

void ToolRegistry::SetWorkspace(const std::string& path) {
  workspace_path_ = path;
}

void ToolRegistry::SetScopeValidator(
    std::shared_ptr<ScopeValidator> validator) {
  scope_validator_ = std::move(validator);
}

void ToolRegistry::SetReconRuntime(ReconRuntime* runtime) {
  recon_runtime_ = runtime;
}

// ---------------------------------------------------------------------------
// RegisterExternalTool
// ---------------------------------------------------------------------------

void ToolRegistry::RegisterExternalTool(
    const std::string& name, const std::string& description,
    const nlohmann::json& parameters,
    std::function<std::string(const nlohmann::json&)> executor) {
  // This is the runtime writer: MCP servers call it on every (re)connect while
  // agent threads are concurrently reading the registry.
  register_tool(name, description, parameters, std::move(executor));
  {
    std::unique_lock lock(registry_mu_);
    external_tools_.insert(name);
  }
  logger_->info("Registered external tool: {}", name);
}

// ---------------------------------------------------------------------------
// Permission checks / ExecuteTool / GetToolSchemas / HasTool
// ---------------------------------------------------------------------------

bool ToolRegistry::permission_allows(const std::string& tool_name,
                                     bool is_external) const {
  if (!permission_checker_)
    return true;
  if (is_external && mcp_tool_manager_) {
    return permission_checker_->IsMcpToolAllowed(
        mcp_tool_manager_->GetServerName(tool_name),
        mcp_tool_manager_->GetOriginalToolName(tool_name));
  }
  return permission_checker_->IsAllowed(tool_name);
}

bool ToolRegistry::check_permission(const std::string& tool_name) const {
  bool is_external;
  {
    std::shared_lock lock(registry_mu_);
    is_external = external_tools_.count(tool_name) > 0;
  }
  return permission_allows(tool_name, is_external);
}

std::string ToolRegistry::ExecuteTool(const std::string& tool_name,
                                      const nlohmann::json& parameters) {
  // Resolve everything the registry owns in one shared-lock section, then run
  // the handler with the lock released. Copying the std::function out matters
  // for more than deadlock avoidance: a reference into tools_ would dangle if
  // an MCP reconnect re-registered the tool mid-call.
  std::function<std::string(const nlohmann::json&)> handler;
  bool is_external = false;
  bool declared_mutating = false;
  {
    std::shared_lock lock(registry_mu_);
    auto it = tools_.find(tool_name);
    if (it == tools_.end())
      throw std::runtime_error("Tool not found: " + tool_name);
    handler = it->second;
    is_external = external_tools_.count(tool_name) > 0;
    declared_mutating = tool_is_mutating_locked(tool_name);
  }

  if (!permission_allows(tool_name, is_external))
    throw std::runtime_error("Permission denied: tool '" + tool_name +
                             "' is not allowed");
  // Scope enforcement gate — validates targets before execution.
  if (scope_validator_ && scope_validator_->IsEnabled()) {
    auto scope_error = scope_validator_->ValidateToolCall(tool_name, parameters);
    if (!scope_error.empty()) {
      logger_->error("Scope violation for tool '{}': {}", tool_name,
                     scope_error);
      throw std::runtime_error("Scope violation: " + scope_error);
    }
  }

  // exec/bash run their own approval prompt inside exec_tool with the actual
  // command line, which is more informative than a generic tool summary.
  if (approval_manager_ && tool_name != "exec" && tool_name != "bash" &&
      is_mutating_impl(tool_name, parameters, is_external,
                       declared_mutating)) {
    auto decision =
        approval_manager_->RequestApproval(approval_summary(tool_name, parameters));
    if (decision == ApprovalDecision::kDenied) {
      throw std::runtime_error("Tool execution denied: " + tool_name);
    }
    if (decision == ApprovalDecision::kTimeout) {
      throw std::runtime_error("Tool approval timed out: " + tool_name);
    }
  }
  logger_->debug("Executing tool: {} params: {}", tool_name, parameters.dump());
  try {
    auto result = handler(parameters);
    logger_->debug("Tool {} succeeded", tool_name);
    return result;
  } catch (const std::exception& e) {
    logger_->error("Tool {} failed: {}", tool_name, e.what());
    throw;
  }
}

std::vector<ToolRegistry::ToolSchema> ToolRegistry::GetToolSchemas() const {
  std::shared_lock lock(registry_mu_);
  if (!permission_checker_)
    return tool_schemas_;
  std::vector<ToolSchema> filtered;
  filtered.reserve(tool_schemas_.size());
  for (const auto& schema : tool_schemas_) {
    if (external_tools_.count(schema.name) && mcp_tool_manager_) {
      if (permission_checker_->IsMcpToolAllowed(
              mcp_tool_manager_->GetServerName(schema.name),
              mcp_tool_manager_->GetOriginalToolName(schema.name)))
        filtered.push_back(schema);
    } else {
      if (permission_checker_->IsAllowed(schema.name))
        filtered.push_back(schema);
    }
  }
  return filtered;
}

bool ToolRegistry::HasTool(const std::string& tool_name) const {
  std::shared_lock lock(registry_mu_);
  return tools_.find(tool_name) != tools_.end();
}

bool ToolRegistry::looks_like_network_write(const nlohmann::json& params) {
  if (!params.is_object())
    return false;

  auto method_it = params.find("method");
  if (method_it == params.end() || !method_it->is_string())
    return false;

  std::string method = method_it->get<std::string>();
  std::transform(method.begin(), method.end(), method.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  return method == "POST" || method == "PUT" || method == "PATCH" ||
         method == "DELETE";
}

bool ToolRegistry::looks_like_mutating_action(const nlohmann::json& params) {
  if (!params.is_object())
    return false;

  auto action_it = params.find("action");
  if (action_it == params.end() || !action_it->is_string())
    return false;

  std::string action = action_it->get<std::string>();
  std::transform(action.begin(), action.end(), action.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  return action == "add" || action == "create" || action == "delete" ||
         action == "remove" || action == "update" || action == "set" ||
         action == "install" || action == "enable" || action == "disable" ||
         action == "write" || action == "send-keys" || action == "start" ||
         action == "stop" || action == "run" || action == "kill";
}

std::string ToolRegistry::approval_summary(const std::string& tool_name,
                                           const nlohmann::json& params) {
  std::string payload = params.dump();
  if (payload.size() > 240) {
    payload = payload.substr(0, 240) + "...";
  }
  return "tool:" + tool_name + " args=" + payload;
}

bool ToolRegistry::IsMutatingToolCall(const std::string& tool_name,
                                      const nlohmann::json& parameters) const {
  bool is_external;
  bool declared;
  {
    std::shared_lock lock(registry_mu_);
    is_external = external_tools_.count(tool_name) > 0;
    declared = tool_is_mutating_locked(tool_name);
  }
  return is_mutating_impl(tool_name, parameters, is_external, declared);
}

bool ToolRegistry::is_mutating_impl(const std::string& tool_name,
                                    const nlohmann::json& parameters,
                                    bool is_external, bool declared_mutating) {
  // Declared at registration — the authoritative answer for built-in tools.
  if (declared_mutating) {
    return true;
  }

  // `process` is genuinely conditional: whether it mutates depends on the
  // requested action, so it cannot be settled at registration time.
  if (tool_name == "process") {
    return looks_like_mutating_action(parameters);
  }

  // External (MCP) tools carry no such declaration, so the name/parameter
  // heuristics remain the only signal available for them.
  if (is_external) {
    if (looks_like_network_write(parameters) ||
        looks_like_mutating_action(parameters)) {
      return true;
    }

    std::string lowered = tool_name;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return lowered.find("write") != std::string::npos ||
           lowered.find("create") != std::string::npos ||
           lowered.find("update") != std::string::npos ||
           lowered.find("delete") != std::string::npos ||
           lowered.find("install") != std::string::npos ||
           lowered.find("enable") != std::string::npos ||
           lowered.find("disable") != std::string::npos ||
           lowered.find("patch") != std::string::npos ||
           lowered.find("remove") != std::string::npos;
  }

  return false;
}

}  // namespace quantclaw
