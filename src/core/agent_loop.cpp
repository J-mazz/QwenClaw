// Copyright 2025 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0

module;

#include <spdlog/spdlog.h>

module quantclaw.core.agent_loop;

import std;

import nlohmann.json;
import quantclaw.constants;
import quantclaw.providers.failover_resolver;
import quantclaw.core.context_engine;
import quantclaw.core.content_block;
import quantclaw.core.dag_runtime;
import quantclaw.core.usage_accumulator;
import quantclaw.providers.provider_error;
import quantclaw.core.context_pruner;
import quantclaw.core.default_context_engine;
import quantclaw.core.memory_manager;
import quantclaw.core.session_compaction;
import quantclaw.core.skill_loader;
import quantclaw.providers.provider_registry;
import quantclaw.providers.llm_provider;
import quantclaw.gateway.protocol;
import quantclaw.providers.failover_resolver;
import quantclaw.tools.tool_registry;

// Bring event name constants into scope
namespace events = quantclaw::gateway::events;

namespace quantclaw {

// Exponential backoff with full jitter, capped. Honours a server-supplied
// Retry-After when there is one — guessing shorter than the server asked just
// earns another 429.
static std::chrono::milliseconds retry_backoff(int attempt,
                                               int retry_after_seconds) {
  if (retry_after_seconds > 0) {
    return std::chrono::milliseconds(
        std::min(retry_after_seconds * 1000, kRetryBackoffMaxMs));
  }
  long long base = static_cast<long long>(kRetryBackoffBaseMs)
                   << std::min(attempt, 5);
  long long capped = std::min<long long>(base, kRetryBackoffMaxMs);
  // Full jitter: without it, every concurrent turn that hit the same outage
  // retries at the same instant and re-creates the spike.
  thread_local std::mt19937 rng(std::random_device{}());
  std::uniform_int_distribution<long long> dist(capped / 2, capped);
  return std::chrono::milliseconds(dist(rng));
}

static int compute_effective_max_iterations(const AgentConfig& cfg) {
  // Respect explicit user overrides (e.g. local orchestration tuning).
  // Keep dynamic OpenClaw scaling only when the default value is in use.
  if (cfg.max_iterations > 0 && cfg.max_iterations != kDefaultMaxIterations) {
    return cfg.max_iterations;
  }
  return cfg.DynamicMaxIterations();
}

// Truncate a tool result if it exceeds the limit (head + tail with ellipsis).
// Uses iterator-based line counting to avoid O(S) heap allocation for the
// full line vector; only the tail window of keep_lines offsets is stored.
static std::string truncate_tool_result(const std::string& result,
                                        int max_chars, int keep_lines) {
  if (static_cast<int>(result.size()) <= max_chars)
    return result;

  // First pass: count total lines and record the byte-offset of each newline
  // boundary. We only need the last keep_lines offsets, so maintain a circular
  // buffer of size keep_lines — O(keep_lines) space rather than O(S).
  int total_lines = 0;
  // tail_offsets[i] = start position of the (total_lines - keep_lines + i)-th line
  std::vector<std::size_t> tail_offsets(static_cast<std::size_t>(keep_lines), 0);
  std::size_t pos = 0;
  std::size_t head_end = 0; // byte position just after the keep_lines-th '\n'

  while (pos <= result.size()) {
    std::size_t nl = result.find('\n', pos);
    if (nl == std::string::npos)
      nl = result.size();

    // Record start of this line in the circular tail buffer
    tail_offsets[static_cast<std::size_t>(total_lines) %
                 static_cast<std::size_t>(keep_lines)] = pos;

    total_lines++;
    if (total_lines == keep_lines)
      head_end = nl + 1; // first keep_lines lines end here

    pos = nl + 1;
    if (nl == result.size())
      break;
  }

  if (total_lines <= keep_lines * 2) {
    // Too few lines to trim by line, but we are here because the payload
    // already exceeds max_chars — a single 50 MB line would otherwise sail
    // through untouched and land in the next request. Fall back to a byte
    // window over the same head/tail shape.
    std::size_t half = static_cast<std::size_t>(max_chars) / 2;
    std::size_t omitted_bytes = result.size() - 2 * half;
    std::string out;
    out.reserve(static_cast<std::size_t>(max_chars) + 64);
    out.append(result, 0, half);
    out += "\n... [" + std::to_string(omitted_bytes) +
           " characters omitted] ...\n\n";
    out.append(result, result.size() - half, std::string::npos);
    return out;
  }

  int omitted = total_lines - keep_lines * 2;

  // Head: result[0..head_end)
  std::string truncated;
  truncated.reserve(head_end + 64 +
                    result.size() - tail_offsets[static_cast<std::size_t>(
                                        total_lines) %
                                    static_cast<std::size_t>(keep_lines)]);
  truncated.append(result, 0, head_end);
  truncated += "\n... [" + std::to_string(omitted) + " lines omitted] ...\n\n";

  // Tail: oldest entry in the circular buffer is where the tail window starts
  std::size_t tail_start =
      tail_offsets[static_cast<std::size_t>(total_lines) %
                   static_cast<std::size_t>(keep_lines)];
  truncated.append(result, tail_start, std::string::npos);
  return truncated;
}

static size_t estimate_request_payload_chars(const std::vector<Message>& msgs) {
  size_t total = 0;
  for (const auto& msg : msgs) {
    total += msg.role.size();
    for (const auto& block : msg.content) {
      total += block.type.size();
      total += block.text.size();
      total += block.id.size();
      total += block.name.size();
      total += block.tool_use_id.size();
      total += block.content.size();
      if (!block.input.is_null()) {
        total += block.input.dump().size();
      }
    }
  }
  return total;
}

static void emit_memory_management_node(DagRuntime* dag_runtime,
                                        DagTurnState* dag_turn,
                                        const AgentConfig& cfg,
                                        const std::shared_ptr<MemoryManager>& mm,
                                        int iteration, int max_iterations,
                                        int context_window,
                                        const std::vector<Message>& msgs) {
  if (!dag_runtime || !dag_runtime->IsEnabled() || !dag_turn) {
    return;
  }

  nlohmann::json payload = {
      {"iteration", iteration},
      {"maxIterations", max_iterations},
      {"requestMessages", msgs.size()},
      {"estimatedPayloadChars", estimate_request_payload_chars(msgs)},
      {"contextWindow", context_window},
      {"autoCompact", cfg.auto_compact},
      {"compactMaxMessages", cfg.compact_max_messages},
      {"compactKeepRecent", cfg.compact_keep_recent},
      {"compactMaxTokens", cfg.compact_max_tokens},
      {"hasMemoryManager", static_cast<bool>(mm)},
  };

  if (mm) {
    payload["workspacePath"] = mm->GetWorkspacePath().string();
  }

  dag_runtime->EmitNode(dag_turn, DagNodeType::kMemoryManagement, payload);
}

// Get context window size for a model name.
// Model names may come from GGUF filenames (e.g. "Qwen3.5-9B.Q5_K_M.gguf")
// so matching must be case-insensitive.
static int get_context_window(const std::string& model) {
  std::string lower;
  lower.reserve(model.size());
  for (char c : model)
    lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  // Anthropic models
  if (lower.find("claude") != std::string::npos)
    return kContextWindow200K;
  // OpenAI models
  if (lower.find("gpt-4o") != std::string::npos)
    return kContextWindow128K;
  if (lower.find("gpt-4-turbo") != std::string::npos)
    return kContextWindow128K;
  if (lower.find("gpt-4") != std::string::npos)
    return kContextWindow8K;
  if (lower.find("gpt-3.5") != std::string::npos)
    return kContextWindow16K;
  // Qwen
  if (lower.find("qwen") != std::string::npos)
    return kContextWindow128K;
  // DeepSeek
  if (lower.find("deepseek") != std::string::npos)
    return kContextWindow128K;
  // Llama / Mistral
  if (lower.find("llama") != std::string::npos)
    return kContextWindow128K;
  if (lower.find("mistral") != std::string::npos)
    return kContextWindow32K;
  return kDefaultContextWindow;
}

AgentLoop::AgentLoop(std::shared_ptr<MemoryManager> memory_manager,
                     std::shared_ptr<SkillLoader> skill_loader,
                     std::shared_ptr<ToolRegistry> tool_registry,
                     std::shared_ptr<LLMProvider> llm_provider,
                     const AgentConfig& agent_config,
                     std::shared_ptr<spdlog::logger> logger)
    : memory_manager_(memory_manager),
      skill_loader_(skill_loader),
      tool_registry_(tool_registry),
      llm_provider_(llm_provider),
      logger_(logger),
      agent_config_(agent_config) {
  max_iterations_ = compute_effective_max_iterations(agent_config_);
  logger_->info("AgentLoop initialized with model: {}, max_iterations: {}",
                agent_config_.model, max_iterations_);
}

std::vector<nlohmann::json> AgentLoop::build_tool_definitions() const {
  // Tools are opt-in (agent.autoAttachTools). When disabled, the model answers
  // the query directly with no tool schemas attached — tool use is a separate,
  // on-demand capability, not the default path.
  if (!agent_config_.auto_attach_tools) {
    return {};
  }
  auto schemas = tool_registry_->GetToolSchemas();
  std::vector<nlohmann::json> tools;
  tools.reserve(schemas.size());
  for (const auto& schema : schemas) {
    auto params_json =
        nlohmann::json::parse(schema.parameters_json, nullptr, false);
    if (params_json.is_discarded()) {
      params_json = nlohmann::json::object();
    }
    tools.push_back({{"type", "function"},
                     {"function",
                      {{"name", schema.name},
                       {"description", schema.description},
                       {"parameters", std::move(params_json)}}}});
  }
  return tools;
}

ChatCompletionRequest AgentLoop::prepare_request(
    const std::string& message, const std::vector<Message>& history,
    const std::string& system_prompt, bool stream,
    std::shared_ptr<ContextEngine>& engine, int& ctx_window_out,
    DagTurnState* dag_turn) {
  engine = context_engine_
               ? context_engine_
               : std::make_shared<DefaultContextEngine>(agent_config_, logger_);
  ctx_window_out = agent_config_.context_window > 0
                       ? agent_config_.context_window
                       : get_context_window(agent_config_.model);
  auto assembled = engine->Assemble(history, system_prompt, message,
                                    ctx_window_out, agent_config_.max_tokens);
  if (dag_runtime_ && dag_runtime_->IsEnabled()) {
    dag_runtime_->EmitNode(
        dag_turn, DagNodeType::kContextAssembled,
        nlohmann::json{{"historyMessages", history.size()},
                       {"assembledMessages", assembled.messages.size()},
                       {"contextWindow", ctx_window_out}});
  }

  ChatCompletionRequest request;
  request.messages = std::move(assembled.messages);
  request.model = agent_config_.model;
  request.temperature = agent_config_.temperature;
  request.max_tokens = agent_config_.max_tokens;
  request.thinking = agent_config_.thinking;
  request.stream = stream;
  request.tools = build_tool_definitions();
  request.tool_choice_auto = !request.tools.empty();
  return request;
}

bool AgentLoop::handle_provider_error(
    const ProviderError& pe, DagTurnState* dag_turn,
    ChatCompletionRequest& request, const std::string& system_prompt,
    const std::shared_ptr<ContextEngine>& engine,
    const std::string& original_model, std::shared_ptr<LLMProvider>& provider,
    int& overflow_retries, int& transient_retries) {
  const bool dag = dag_runtime_ && dag_runtime_->IsEnabled();

  if (pe.Kind() == ProviderErrorKind::kContextOverflow) {
    if (overflow_retries >= kOverflowCompactionMaxRetries) {
      logger_->error("Context overflow: all {} compaction retries exhausted",
                     kOverflowCompactionMaxRetries);
      return false;
    }
    overflow_retries++;
    logger_->warn("Context overflow (attempt {}/{}), compacting and retrying",
                  overflow_retries, kOverflowCompactionMaxRetries);
    if (dag) {
      dag_runtime_->EmitNode(
          dag_turn, DagNodeType::kCompaction,
          nlohmann::json{{"attempt", overflow_retries},
                         {"maxRetries", kOverflowCompactionMaxRetries}});
    }
    request.messages = engine->CompactOverflow(request.messages, system_prompt, 0);
    return true;
  }

  // Try a different profile or fallback model before burning a retry.
  if (failover_resolver_ && !last_provider_id_.empty()) {
    failover_resolver_->RecordFailure(last_provider_id_, last_profile_id_,
                                      pe.Kind(), pe.RetryAfterSeconds());
    logger_->warn("Provider error ({}), attempting failover: {}",
                  ProviderErrorKindToString(pe.Kind()), pe.what());
    agent_config_.model = original_model;
    auto new_provider = resolve_provider();
    if (new_provider && new_provider != provider) {
      provider = new_provider;
      request.model = agent_config_.model;
      return true;
    }
  }

  // Only errors that can plausibly succeed on a retry are retried. Retrying a
  // malformed request or an auth failure just burns the budget and delays the
  // real error by up to a minute of blocking sleeps.
  if (!IsRetryable(pe.Kind()) ||
      transient_retries >= kTransientErrorMaxRetries) {
    logger_->error("Provider error with no failover available: {}", pe.what());
    return false;
  }

  // Retries have their own budget: charging them to max_iterations meant a few
  // transient 5xx could consume the entire reasoning allowance for the turn.
  int attempt = transient_retries++;
  auto delay = retry_backoff(attempt, pe.RetryAfterSeconds());
  logger_->warn("Transient provider error ({}), retry {}/{} in {}ms: {}",
                ProviderErrorKindToString(pe.Kind()), transient_retries,
                kTransientErrorMaxRetries, delay.count(), pe.what());
  std::this_thread::sleep_for(delay);
  return true;
}

std::shared_ptr<LLMProvider> AgentLoop::resolve_provider() {
  // If failover resolver is available, use it for profile rotation + fallback
  if (failover_resolver_) {
    auto resolved =
        failover_resolver_->Resolve(agent_config_.model, session_key_);
    if (resolved) {
      last_provider_id_ = resolved->provider_id;
      last_profile_id_ = resolved->profile_id;
      // Update model to the resolved model name (may differ if fallback)
      agent_config_.model = resolved->model;
      if (resolved->is_fallback) {
        logger_->info("Using fallback model: {}/{}", resolved->provider_id,
                      resolved->model);
      }
      return resolved->provider;
    }
    logger_->error("FailoverResolver exhausted all models/profiles for '{}'",
                   agent_config_.model);
    // Fall through to registry / injected provider
  }

  if (!provider_registry_) {
    return llm_provider_;
  }

  auto ref = provider_registry_->ResolveModel(agent_config_.model);
  auto provider = provider_registry_->GetProviderForModel(ref);
  if (provider) {
    last_provider_id_ = ref.provider;
    last_profile_id_ = "";
    // Update model to stripped name (without provider prefix)
    agent_config_.model = ref.model;
    return provider;
  }

  logger_->warn(
      "Failed to resolve provider for model '{}', falling back to injected "
      "provider",
      agent_config_.model);
  return llm_provider_;
}

void AgentLoop::SetModel(const std::string& model_ref) {
  agent_config_.model = model_ref;
  logger_->info("Model set to: {}", model_ref);
}

std::vector<Message> AgentLoop::ProcessMessage(
    const std::string& message, const std::vector<Message>& history,
    const std::string& system_prompt, const std::string& usage_session_key) {
  const std::string& effective_session_key =
      usage_session_key.empty() ? session_key_ : usage_session_key;
  logger_->info("Processing message (non-streaming)");
  stop_requested_ = false;

  DagTurnState dag_turn;
  if (dag_runtime_ && dag_runtime_->IsEnabled()) {
    dag_turn = dag_runtime_->BeginTurn(effective_session_key, message);
  }

  auto provider = resolve_provider();
  if (dag_runtime_ && dag_runtime_->IsEnabled()) {
    dag_runtime_->EmitNode(
        &dag_turn, DagNodeType::kProviderResolved,
        nlohmann::json{{"model", agent_config_.model},
                       {"providerId", last_provider_id_},
                       {"profileId", last_profile_id_}});
  }

  std::vector<Message> new_messages;

  std::shared_ptr<ContextEngine> engine;
  int ctx_window = 0;
  auto request = prepare_request(message, history, system_prompt,
                                 /*stream=*/false, engine, ctx_window,
                                 &dag_turn);

  // Save original model for failover re-resolution
  std::string original_model = agent_config_.model;
  int iterations = 0;
  int overflow_retries = 0;
  int transient_retries = 0;

  while (iterations < max_iterations_ && !stop_requested_) {
    emit_memory_management_node(dag_runtime_.get(), &dag_turn, agent_config_,
                                memory_manager_, iterations + 1,
                                max_iterations_, ctx_window, request.messages);
    try {
      auto response = provider->ChatCompletion(request);
      if (dag_runtime_ && dag_runtime_->IsEnabled()) {
        dag_runtime_->EmitNode(
            &dag_turn, DagNodeType::kLlmResponse,
            nlohmann::json{{"hasToolCalls", !response.tool_calls.empty()},
                           {"toolCallCount", response.tool_calls.size()},
                           {"contentSize", response.content.size()}});
      }

      // --- Usage tracking ---
      if (usage_accumulator_ && !effective_session_key.empty()) {
        usage_accumulator_->Record(effective_session_key,
                                   response.usage.prompt_tokens,
                                   response.usage.completion_tokens);
      }
      logger_->debug("Token usage: prompt={} completion={}",
                     response.usage.prompt_tokens,
                     response.usage.completion_tokens);

      // Record success for failover tracking
      if (failover_resolver_ && !last_provider_id_.empty()) {
        failover_resolver_->RecordSuccess(last_provider_id_, last_profile_id_,
                                          session_key_);
      }

      if (!response.tool_calls.empty()) {
        logger_->info("LLM requested {} tool calls",
                      response.tool_calls.size());

        if (dag_runtime_ && dag_runtime_->IsEnabled()) {
          for (const auto& tc : response.tool_calls) {
            dag_runtime_->EmitNode(
                &dag_turn, DagNodeType::kToolCall,
                nlohmann::json{{"id", tc.id},
                               {"name", tc.name},
                               {"input", tc.arguments}});
          }
        }

        std::vector<nlohmann::json> tool_calls_json;
        for (const auto& tc : response.tool_calls) {
          nlohmann::json tc_json;
          tc_json["id"] = tc.id;
          tc_json["function"]["name"] = tc.name;
          tc_json["function"]["arguments"] = tc.arguments.dump();
          tool_calls_json.push_back(tc_json);
        }
        auto tool_results = handle_tool_calls(tool_calls_json);

        // --- Tool result truncation fallback ---
        for (auto& result : tool_results) {
          result = truncate_tool_result(result, kToolResultMaxChars,
                                        kToolResultKeepLines);
          if (dag_runtime_ && dag_runtime_->IsEnabled()) {
            dag_runtime_->EmitNode(
                &dag_turn, DagNodeType::kToolResult,
                nlohmann::json{{"contentSize", result.size()}});
          }
        }

        // Assistant message: text + tool_use blocks
        Message assistant_msg;
        assistant_msg.role = "assistant";
        if (!response.content.empty())
          assistant_msg.content.push_back(
              ContentBlock::MakeText(response.content));
        for (const auto& tc : response.tool_calls)
          assistant_msg.content.push_back(
              ContentBlock::MakeToolUse(tc.id, tc.name, tc.arguments));
        request.messages.push_back(assistant_msg);
        new_messages.push_back(assistant_msg);

        // Tool results: single user message with tool_result blocks
        Message results_msg;
        results_msg.role = "user";
        for (size_t i = 0; i < response.tool_calls.size(); i++)
          results_msg.content.push_back(ContentBlock::MakeToolResult(
              response.tool_calls[i].id, tool_results[i]));
        request.messages.push_back(results_msg);
        new_messages.push_back(results_msg);

        iterations++;
        continue;
      }

      if (!response.content.empty()) {
        logger_->info("LLM provided final response");
        if (dag_runtime_ && dag_runtime_->IsEnabled()) {
          dag_runtime_->EmitNode(
              &dag_turn, DagNodeType::kTurnFinal,
              nlohmann::json{{"contentSize", response.content.size()}});
          dag_runtime_->EndTurn(&dag_turn, "completed");
        }
        Message final_msg;
        final_msg.role = "assistant";
        final_msg.content.push_back(ContentBlock::MakeText(response.content));
        new_messages.push_back(final_msg);
        return new_messages;
      }

      logger_->error("Unexpected LLM response format");
      if (dag_runtime_ && dag_runtime_->IsEnabled()) {
        dag_runtime_->EmitNode(&dag_turn, DagNodeType::kTurnError,
                               nlohmann::json{{"reason", "unexpected_response_format"}});
      }
      break;

    } catch (const ProviderError& pe) {
      if (handle_provider_error(pe, &dag_turn, request, system_prompt, engine,
                                original_model, provider, overflow_retries,
                                transient_retries)) {
        continue;
      }
      if (dag_runtime_ && dag_runtime_->IsEnabled()) {
        dag_runtime_->EmitNode(
            &dag_turn, DagNodeType::kTurnError,
            nlohmann::json{{"providerErrorKind",
                            ProviderErrorKindToString(pe.Kind())},
                           {"error", pe.what()}});
        dag_runtime_->EndTurn(&dag_turn, "error", pe.what());
      }
      throw;

    } catch (const std::exception& e) {
      // Anything not classified as a ProviderError is a bug or a local
      // failure, not a flaky endpoint — retrying it just delays the report.
      logger_->error("Error in LLM processing: {}", e.what());
      if (dag_runtime_ && dag_runtime_->IsEnabled()) {
        dag_runtime_->EmitNode(&dag_turn, DagNodeType::kTurnError,
                               nlohmann::json{{"error", e.what()}});
        dag_runtime_->EndTurn(&dag_turn, "error", e.what());
      }
      throw;
    }
  }

  if (stop_requested_) {
    if (dag_runtime_ && dag_runtime_->IsEnabled()) {
      dag_runtime_->EmitNode(&dag_turn, DagNodeType::kTurnStopped,
                             nlohmann::json::object());
      dag_runtime_->EndTurn(&dag_turn, "stopped");
    }
    Message stop_msg;
    stop_msg.role = "assistant";
    stop_msg.content.push_back(
        ContentBlock::MakeText("[Agent turn stopped by user]"));
    new_messages.push_back(stop_msg);
    return new_messages;
  }

  if (dag_runtime_ && dag_runtime_->IsEnabled()) {
    dag_runtime_->EmitNode(
        &dag_turn, DagNodeType::kTurnError,
        nlohmann::json{{"error", "max_iterations_exhausted"},
                       {"maxIterations", max_iterations_}});
    dag_runtime_->EndTurn(&dag_turn, "error", "max_iterations_exhausted");
  }

  throw std::runtime_error("Failed to get valid response after " +
                           std::to_string(max_iterations_) + " iterations");
}

std::vector<Message> AgentLoop::ProcessMessageStream(
    const std::string& message, const std::vector<Message>& history,
    const std::string& system_prompt, AgentEventCallback callback,
    const std::string& usage_session_key) {
  const std::string& effective_session_key =
      usage_session_key.empty() ? session_key_ : usage_session_key;
  logger_->info("Processing message (streaming)");
  stop_requested_ = false;

  DagTurnState dag_turn;
  if (dag_runtime_ && dag_runtime_->IsEnabled()) {
    dag_turn = dag_runtime_->BeginTurn(effective_session_key, message);
  }

  auto provider = resolve_provider();
  if (dag_runtime_ && dag_runtime_->IsEnabled()) {
    dag_runtime_->EmitNode(
        &dag_turn, DagNodeType::kProviderResolved,
        nlohmann::json{{"model", agent_config_.model},
                       {"providerId", last_provider_id_},
                       {"profileId", last_profile_id_}});
  }

  std::vector<Message> new_messages;

  std::shared_ptr<ContextEngine> engine;
  int ctx_window = 0;
  auto request = prepare_request(message, history, system_prompt,
                                 /*stream=*/true, engine, ctx_window,
                                 &dag_turn);

  std::string original_model_stream = agent_config_.model;
  int iterations = 0;
  int overflow_retries_stream = 0;
  int transient_retries_stream = 0;

  while (iterations < max_iterations_ && !stop_requested_) {
    emit_memory_management_node(dag_runtime_.get(), &dag_turn, agent_config_,
                                memory_manager_, iterations + 1,
                                max_iterations_, ctx_window, request.messages);
    try {
      std::string full_response;
      std::string full_reasoning;  // reasoning-model "thinking" output
      std::vector<ToolCall> pending_tool_calls;
      TokenUsage stream_usage;

      // The callback only accumulates. Tools used to be executed right here,
      // inside the SSE parse callback, while the HTTP response was still open:
      // a single 300s exec stalled the provider's read loop for its whole
      // duration, and the request messages were mutated mid-request. Collect
      // the calls, let the stream close, then run them.
      provider->ChatCompletionStream(
          request, [&](const ChatCompletionResponse& chunk) {
            if (stop_requested_) {
              // Checked here as well as at the loop head, or a stop issued
              // during a long generation is not honoured until it finishes.
              return;
            }

            if (!chunk.reasoning_content.empty()) {
              full_reasoning += chunk.reasoning_content;
              if (callback) {
                callback({events::kThinkingDelta,
                          {{"text", chunk.reasoning_content}}});
              }
            }

            if (!chunk.content.empty()) {
              full_response += chunk.content;
              if (callback) {
                callback({events::kTextDelta, {{"text", chunk.content}}});
              }
            }

            // Accumulate usage from stream chunks
            stream_usage.prompt_tokens += chunk.usage.prompt_tokens;
            stream_usage.completion_tokens += chunk.usage.completion_tokens;

            for (const auto& tc : chunk.tool_calls) {
              // Surface the intent immediately so the UI stays live; execution
              // happens once the stream is done.
              if (dag_runtime_ && dag_runtime_->IsEnabled()) {
                dag_runtime_->EmitNode(&dag_turn, DagNodeType::kToolCall,
                                       nlohmann::json{{"id", tc.id},
                                                      {"name", tc.name},
                                                      {"input", tc.arguments}});
              }
              if (callback) {
                callback({events::kToolUse,
                          {{"id", tc.id},
                           {"name", tc.name},
                           {"input", tc.arguments}}});
              }
              pending_tool_calls.push_back(tc);
            }

            if (chunk.is_stream_end && pending_tool_calls.empty()) {
              if (callback) {
                callback({events::kMessageEnd, {{"content", full_response}}});
              }
            }
          });

      // --- Usage tracking ---
      if (usage_accumulator_ && !effective_session_key.empty()) {
        usage_accumulator_->Record(effective_session_key,
                                   stream_usage.prompt_tokens,
                                   stream_usage.completion_tokens);
      }
      logger_->debug("Token usage (stream): prompt={} completion={}",
                     stream_usage.prompt_tokens,
                     stream_usage.completion_tokens);

      // Record success for failover tracking
      if (failover_resolver_ && !last_provider_id_.empty()) {
        failover_resolver_->RecordSuccess(last_provider_id_, last_profile_id_,
                                          session_key_);
      }

      // --- Tool calls: executed now that the stream is closed ---
      if (!pending_tool_calls.empty()) {
        // One assistant message carrying the text plus every tool_use block,
        // then one user message carrying every tool_result — matching the
        // non-streaming path. The old code emitted a separate assistant/user
        // pair per tool call and attached the assistant text only to the
        // first, which is not a shape the message APIs expect.
        Message assistant_msg;
        assistant_msg.role = "assistant";
        if (!full_response.empty()) {
          assistant_msg.content.push_back(
              ContentBlock::MakeText(full_response));
        }
        for (const auto& tc : pending_tool_calls) {
          assistant_msg.content.push_back(
              ContentBlock::MakeToolUse(tc.id, tc.name, tc.arguments));
        }
        request.messages.push_back(assistant_msg);
        new_messages.push_back(assistant_msg);

        Message results_msg;
        results_msg.role = "user";
        for (const auto& tc : pending_tool_calls) {
          std::string content;
          bool is_error = false;
          try {
            content = truncate_tool_result(
                tool_registry_->ExecuteTool(tc.name, tc.arguments),
                kToolResultMaxChars, kToolResultKeepLines);
          } catch (const nlohmann::json::exception& e) {
            content = "JSON serialization error: " + std::string(e.what());
            is_error = true;
            logger_->error("Tool '{}' JSON error: {}", tc.name, e.what());
          } catch (const std::exception& e) {
            content = "Error: " + std::string(e.what());
            is_error = true;
            logger_->error("Tool '{}' failed: {}", tc.name, e.what());
          }

          if (dag_runtime_ && dag_runtime_->IsEnabled()) {
            dag_runtime_->EmitNode(&dag_turn, DagNodeType::kToolResult,
                                   nlohmann::json{{"toolUseId", tc.id},
                                                  {"contentSize", content.size()},
                                                  {"isError", is_error}});
          }
          if (callback) {
            nlohmann::json data{{"tool_use_id", tc.id}, {"content", content}};
            if (is_error) {
              data["is_error"] = true;
            }
            callback({events::kToolResult, data});
          }
          results_msg.content.push_back(
              ContentBlock::MakeToolResult(tc.id, content));
        }
        request.messages.push_back(results_msg);
        new_messages.push_back(results_msg);

        // Exactly one iteration per model turn. This used to be incremented
        // once inside the callback per tool-call chunk *and* again here, so a
        // tool-using turn burned at least two of the budget.
        iterations++;
        continue;
      }

      // If we got a final text response without tool calls, we're done.
      if (!full_response.empty()) {
        if (dag_runtime_ && dag_runtime_->IsEnabled()) {
          dag_runtime_->EmitNode(
              &dag_turn, DagNodeType::kTurnFinal,
              nlohmann::json{{"contentSize", full_response.size()}});
          dag_runtime_->EndTurn(&dag_turn, "completed");
        }
        Message final_msg;
        final_msg.role = "assistant";
        final_msg.content.push_back(ContentBlock::MakeText(full_response));
        new_messages.push_back(final_msg);
        return new_messages;
      }

      // No assistant text and no tool call this turn — the model produced
      // nothing actionable (e.g. a reasoning model whose answer landed in
      // reasoning_content). Terminate instead of spinning to max_iterations;
      // surface the reasoning as the answer so the turn isn't silent.
      if (dag_runtime_ && dag_runtime_->IsEnabled()) {
        dag_runtime_->EmitNode(
            &dag_turn, DagNodeType::kTurnFinal,
            nlohmann::json{{"contentSize", full_reasoning.size()},
                           {"fromReasoning", true}});
        dag_runtime_->EndTurn(&dag_turn, "completed");
      }
      if (!full_reasoning.empty()) {
        if (callback) {
          callback({events::kTextDelta, {{"text", full_reasoning}}});
        }
        Message final_msg;
        final_msg.role = "assistant";
        final_msg.content.push_back(ContentBlock::MakeText(full_reasoning));
        new_messages.push_back(final_msg);
      }
      if (callback) {
        callback({events::kMessageEnd, {{"content", full_reasoning}}});
      }
      return new_messages;

    } catch (const ProviderError& pe) {
      if (handle_provider_error(pe, &dag_turn, request, system_prompt, engine,
                                original_model_stream, provider,
                                overflow_retries_stream,
                                transient_retries_stream)) {
        continue;
      }

      logger_->error("Error in streaming: {}", pe.what());
      if (dag_runtime_ && dag_runtime_->IsEnabled()) {
        dag_runtime_->EmitNode(
            &dag_turn, DagNodeType::kTurnError,
            nlohmann::json{{"providerErrorKind",
                            ProviderErrorKindToString(pe.Kind())},
                           {"error", pe.what()}});
        dag_runtime_->EndTurn(&dag_turn, "error", pe.what());
      }
      if (callback) {
        callback({events::kMessageEnd, {{"error", pe.what()}}});
      }
      return new_messages;

    } catch (const std::exception& e) {
      logger_->error("Error in streaming: {}", e.what());
      if (dag_runtime_ && dag_runtime_->IsEnabled()) {
        dag_runtime_->EmitNode(&dag_turn, DagNodeType::kTurnError,
                               nlohmann::json{{"error", e.what()}});
        dag_runtime_->EndTurn(&dag_turn, "error", e.what());
      }
      if (callback) {
        callback({events::kMessageEnd, {{"error", e.what()}}});
      }
      return new_messages;
    }
  }

  std::string stop_text =
      stop_requested_ ? "[Stopped]" : "[Max iterations reached]";
  if (dag_runtime_ && dag_runtime_->IsEnabled()) {
    dag_runtime_->EmitNode(
        &dag_turn,
        stop_requested_ ? DagNodeType::kTurnStopped : DagNodeType::kTurnError,
        nlohmann::json{{"content", stop_text}});
    dag_runtime_->EndTurn(&dag_turn, stop_requested_ ? "stopped" : "error",
                          stop_requested_ ? "" : "max_iterations_reached");
  }
  if (callback) {
    callback({events::kMessageEnd, {{"content", stop_text}}});
  }
  Message stop_msg;
  stop_msg.role = "assistant";
  stop_msg.content.push_back(ContentBlock::MakeText(stop_text));
  new_messages.push_back(stop_msg);
  return new_messages;
}

void AgentLoop::Stop() {
  stop_requested_ = true;
  logger_->info("Agent stop requested");
}

void AgentLoop::SetConfig(const AgentConfig& config) {
  agent_config_ = config;
  max_iterations_ = compute_effective_max_iterations(config);
  logger_->info(
      "AgentLoop config updated: model={}, temp={}, max_tokens={}, "
      "max_iterations={}, thinking={}",
      config.model, config.temperature, config.max_tokens, max_iterations_,
      config.thinking);
}

std::vector<std::string>
AgentLoop::handle_tool_calls(const std::vector<nlohmann::json>& tool_calls) {
  std::vector<std::string> results;

  for (const auto& tool_call : tool_calls) {
    try {
      std::string tool_name = tool_call["function"]["name"];
      nlohmann::json arguments;
      const auto& args_val = tool_call["function"]["arguments"];
      if (args_val.is_string()) {
        arguments = nlohmann::json::parse(args_val.get<std::string>());
      } else {
        arguments = args_val;
      }

      logger_->info("Executing tool: {} with arguments: {}", tool_name,
                    arguments.dump());
      std::string result = tool_registry_->ExecuteTool(tool_name, arguments);
      results.push_back(result);
      logger_->info("Tool execution successful");

    } catch (const nlohmann::json::exception& e) {
      logger_->error("Tool '{}' JSON serialization error: {}",
                     tool_call.value("function", nlohmann::json{})
                               .value("name", std::string{"unknown"}),
                     e.what());
      results.push_back("JSON serialization error: " + std::string(e.what()));
    } catch (const std::exception& e) {
      logger_->error("Tool execution failed: {}", e.what());
      results.push_back("Error executing tool: " + std::string(e.what()));
    }
  }

  return results;
}

}  // namespace quantclaw
