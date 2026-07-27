// Copyright 2026 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0
//
// Process tools: shell execution, the Claude advisor, channel messaging,
// and background process management.
//
// One of several implementation units of quantclaw.tools.tool_registry. The
// registry and its fifteen tool bodies lived in a single 2200-line file; the
// bodies are unchanged, only relocated.

module;

#include <spdlog/spdlog.h>

module quantclaw.tools.tool_registry;

import std;
import nlohmann.json;

import quantclaw.common.defer;
import quantclaw.platform.process;
import quantclaw.security.exec_approval;
import quantclaw.security.sandbox;
import quantclaw.tools.tool_helpers;

namespace fs = std::filesystem;

namespace quantclaw {

using namespace quantclaw::tools;

// ---------------------------------------------------------------------------
// exec_tool
// ---------------------------------------------------------------------------

std::string ToolRegistry::exec_tool(const nlohmann::json& params) {
  if (!params.contains("command"))
    throw std::runtime_error("Missing required parameter: command");
  std::string command = params["command"].get<std::string>();
  int timeout = params.value("timeout", 30);
  std::string workdir = params.value("workdir", "");

  // Ground the shell in the agent workspace by default so relative paths line
  // up with the file tools (read/write/edit). An explicit workdir is still
  // validated to stay inside the workspace.
  std::string resolved_workdir = workspace_path_;
  if (!workdir.empty()) {
    if (!quantclaw::SecuritySandbox::ValidateFilePath(workdir,
                                                      workspace_path_)) {
      throw std::runtime_error("Access denied: workdir outside workspace: " +
                               workdir);
    }
    // Resolve relative workdir against workspace so exec_capture uses
    // the correct directory (not gateway CWD).
    std::error_code ec;
    fs::path ws_abs = fs::weakly_canonical(workspace_path_, ec);
    if (!ec) {
      fs::path wd_path(workdir);
      if (wd_path.is_relative()) {
        wd_path = ws_abs / wd_path;
      }
      fs::path wd_abs = fs::weakly_canonical(wd_path, ec);
      if (!ec) {
        resolved_workdir = wd_abs.string();
      }
    }
  }

  if (!quantclaw::SecuritySandbox::ValidateShellCommand(command)) {
    throw std::runtime_error("Command not allowed: " + command);
  }

  if (approval_manager_) {
    auto decision = approval_manager_->RequestApproval(command);
    if (decision == ApprovalDecision::kDenied) {
      throw std::runtime_error("Command execution denied: " + command);
    }
    if (decision == ApprovalDecision::kTimeout) {
      throw std::runtime_error("Approval timed out: " + command);
    }
  }

  logger_->info("Executing command: {}", command);

  auto result = platform::exec_capture(command, timeout, resolved_workdir);
  if (result.exit_code == -1)
    throw std::runtime_error("Failed to execute: " + command);
  if (result.exit_code == -2)
    throw std::runtime_error("Command timeout: " + command);
  if (result.exit_code != 0)
    throw std::runtime_error("Command exited " +
                             std::to_string(result.exit_code) + ": " +
                             result.output);
  return result.output;
}

// ---------------------------------------------------------------------------
// claude_advisor_tool — consult Claude via the official Claude Code CLI.
//
// Compliance: this shells out to the user's installed `claude` binary in
// headless mode (`-p`).  Claude Code authenticates with its own login;
// QuantClaw never reads, stores, or forwards any credential.  The (arbitrary)
// prompt is delivered via stdin, never interpolated into the shell, and the
// run is tool-less so it can only return written advice.
// ---------------------------------------------------------------------------

static std::string resolve_claude_bin() {
  if (const char* env = std::getenv("QUANTCLAW_CLAUDE_BIN"); env && *env)
    return env;
  if (const char* home = std::getenv("HOME"); home && *home) {
    fs::path p = fs::path(home) / ".local" / "bin" / "claude";
    std::error_code ec;
    if (fs::exists(p, ec))
      return p.string();
  }
  return "claude";
}

std::string ToolRegistry::claude_advisor_tool(const nlohmann::json& params) {
  if (!params.contains("question"))
    throw std::runtime_error("Missing required parameter: question");
  std::string question = params["question"].get<std::string>();
  std::string context = params.value("context", "");
  std::string model = params.value("model", "opus");
  int timeout = params.value("timeout", 300);

  // Whitelist the model so a bogus value can't reach the CLI (it is also
  // shell-quoted below — belt and suspenders).
  if (model != "opus" && model != "sonnet" && model != "haiku")
    throw std::runtime_error("Unsupported advisor model: " + model +
                             " (use opus, sonnet, or haiku)");

  std::string prompt = question;
  if (!context.empty())
    prompt += "\n\n--- Context ---\n" + context;

  // Write the prompt to a temp file so it reaches the CLI via stdin and is
  // never parsed by the shell. The name is unique per call: a fixed name is
  // clobbered when two sessions consult the advisor at once, and each would
  // then read the other's prompt.
  fs::path tmp = fs::path(workspace_path_) /
                 (".claude_advisor_" + generate_id("prompt") + ".txt");
  // Remove the prompt on every exit path, including the throws below and any
  // exception out of exec_capture.
  auto cleanup = MakeDefer([&tmp] {
    std::error_code ec;
    fs::remove(tmp, ec);
  });
  {
    std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
    if (!ofs)
      throw std::runtime_error("claude_advisor: cannot write prompt file");
    ofs << prompt;
  }

  static constexpr const char* kSystem =
      "You are a senior software engineer acting as an on-demand advisor to an "
      "autonomous coding agent. Give precise, actionable guidance: likely root "
      "cause, concrete steps, and code where it helps. Be concise and direct. "
      "You are read-only; do not attempt to modify files or run commands.";

  std::string bin = resolve_claude_bin();
  std::string cmd = shell_quote(bin) + " -p --model " + shell_quote(model) +
                    " --output-format text --allowedTools '' "
                    "--append-system-prompt " +
                    shell_quote(kSystem) + " < " + shell_quote(tmp.string());

  logger_->info("claude_advisor: consulting {} (model {})", bin, model);

  // The Claude CLI is a Node process: it reserves far more virtual address
  // space than the default child ceiling and thinks for minutes on a hard
  // question. Leave RLIMIT_AS unset and let the CPU ceiling follow the
  // caller's timeout, or the advisor is killed before it answers.
  platform::ExecLimits limits;
  limits.max_address_space_bytes = 0;
  limits.max_cpu_seconds = 0;  // derive from `timeout`
  auto result = platform::exec_capture(cmd, timeout, workspace_path_, limits);

  if (result.exit_code == -1)
    throw std::runtime_error(
        "claude_advisor: failed to launch '" + bin +
        "' (is the Claude Code CLI installed and reachable?)");
  if (result.exit_code == -2)
    throw std::runtime_error("claude_advisor: timed out after " +
                             std::to_string(timeout) + "s");
  if (result.exit_code != 0)
    throw std::runtime_error("claude_advisor: CLI exited " +
                             std::to_string(result.exit_code) + ": " +
                             result.output);
  return result.output;
}

// ---------------------------------------------------------------------------
// message_tool
// ---------------------------------------------------------------------------

std::string ToolRegistry::message_tool(const nlohmann::json& params) {
  if (!params.contains("channel") || !params.contains("message"))
    throw std::runtime_error("Missing required parameters: channel, message");
  std::string channel = params["channel"].get<std::string>();
  std::string message = params["message"].get<std::string>();
  logger_->info("Message to channel {}: {}", channel, message);
  return "Message sent to channel: " + channel;
}


// ---------------------------------------------------------------------------
// process_tool — background process management
// ---------------------------------------------------------------------------

std::string ToolRegistry::process_tool(const nlohmann::json& params) {
  std::string action = params.value("action", "list");

  if (action == "list") {
    std::lock_guard<std::mutex> lk(bg_mu_);
    nlohmann::json sessions = nlohmann::json::array();
    for (auto& [sid, sess] : bg_sessions_) {
      std::lock_guard<std::mutex> slk(sess->mu);
      // Poll future without blocking
      if (!sess->exited && sess->future.valid() &&
          sess->future.wait_for(std::chrono::seconds(0)) ==
              std::future_status::ready) {
        try {
          sess->output = sess->future.get();
        } catch (const std::exception& e) {
          sess->error = e.what();
        }
        sess->exited = true;
      }
      sessions.push_back({{"id", sid},
                          {"command", sess->command},
                          {"running", !sess->exited},
                          {"error", sess->error}});
    }
    return nlohmann::json{{"sessions", sessions}}.dump();
  }

  if (action == "start") {
    std::string command = params.value("command", "");
    if (command.empty())
      throw std::runtime_error("command is required for process start");

    auto sess = std::make_shared<BgSession>();
    sess->id = generate_id("proc");
    sess->command = command;
    auto started = std::chrono::system_clock::now();
    sess->started_at = started;

    // Run command asynchronously
    sess->future = std::async(std::launch::async, [command]() -> std::string {
      auto r = platform::exec_capture(command, 300);  // 5-minute max
      if (r.exit_code != 0 && r.exit_code != -1 && r.exit_code != -2) {
        return r.output + "\n[exit " + std::to_string(r.exit_code) + "]";
      }
      return r.output;
    });

    std::string id = sess->id;
    {
      std::lock_guard<std::mutex> lk(bg_mu_);
      bg_sessions_[id] = std::move(sess);
    }
    return nlohmann::json{{"ok", true}, {"id", id}}.dump();
  }

  std::string id = params.value("id", "");
  if (id.empty())
    throw std::runtime_error("id is required for action: " + action);

  std::shared_ptr<BgSession> sess;
  {
    std::lock_guard<std::mutex> lk(bg_mu_);
    auto it = bg_sessions_.find(id);
    if (it == bg_sessions_.end())
      throw std::runtime_error("No session with id: " + id);
    sess = it->second;
  }

  if (action == "log" || action == "poll") {
    int timeout_ms = params.value("timeout", 5000);
    std::lock_guard<std::mutex> slk(sess->mu);
    if (!sess->exited && sess->future.valid()) {
      auto status =
          sess->future.wait_for(std::chrono::milliseconds(timeout_ms));
      if (status == std::future_status::ready) {
        try {
          sess->output = sess->future.get();
        } catch (const std::exception& e) {
          sess->error = e.what();
        }
        sess->exited = true;
      }
    }
    return nlohmann::json{{"id", id},
                          {"running", !sess->exited},
                          {"output", sess->output},
                          {"error", sess->error}}
        .dump();
  }

  if (action == "kill") {
    // Best-effort: mark as done
    std::lock_guard<std::mutex> slk(sess->mu);
    sess->exited = true;
    sess->error = "killed by user";
    return nlohmann::json{{"ok", true}, {"id", id}}.dump();
  }

  if (action == "clear") {
    std::lock_guard<std::mutex> slk(sess->mu);
    sess->output.clear();
    return nlohmann::json{{"ok", true}}.dump();
  }

  if (action == "remove") {
    std::lock_guard<std::mutex> lk(bg_mu_);
    bg_sessions_.erase(id);
    return nlohmann::json{{"ok", true}}.dump();
  }

  if (action == "write" || action == "send-keys") {
    // Cannot write to stdin without pipe infrastructure; acknowledge gracefully
    logger_->warn("process send-keys: stdin write not supported in this build");
    return nlohmann::json{{"ok", false}, {"note", "stdin write not supported"}}
        .dump();
  }

  throw std::runtime_error("Unknown process action: " + action);
}
}  // namespace quantclaw
