// Copyright 2026 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0
//
// Skill status, cron job management, exec-approval requests, and model
// selection.
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

void RegisterSkillAndCronHandlers(GatewayServer& server, const RpcDeps& d) {
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

  // --- skills.status ---
  if (skill_loader) {
    server.RegisterHandler(
        methods::kSkillsStatus,
        [skill_loader, &config,
         logger](const nlohmann::json& /*params*/,
                 const ClientConnection& /*client*/) -> nlohmann::json {
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
                               const ClientConnection& /*client*/) -> nlohmann::json {
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
                         const ClientConnection& /*client*/) -> nlohmann::json {
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
                         const ClientConnection& /*client*/) -> nlohmann::json {
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
                         const ClientConnection& /*client*/) -> nlohmann::json {
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
                 const ClientConnection& /*client*/) -> nlohmann::json {
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
                 const ClientConnection& /*client*/) -> nlohmann::json {
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
                 const ClientConnection& /*client*/) -> nlohmann::json {
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
                               const ClientConnection& /*client*/) -> nlohmann::json {
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
                 const ClientConnection& /*client*/) -> nlohmann::json {
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
                           const ClientConnection& /*client*/) -> nlohmann::json {
        std::string model = params.value("model", "");
        if (model.empty()) {
          throw std::runtime_error("model is required");
        }
        agent_loop->SetModel(model);
        return {{"ok", true}, {"model", model}};
      });
}

}  // namespace quantclaw::gateway
