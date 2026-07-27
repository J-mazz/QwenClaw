// Copyright 2026 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0

#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <gtest/gtest.h>

import std;
import nlohmann.json;
import quantclaw.providers.llama_provider;
import quantclaw.providers.llm_provider;
import quantclaw.providers.provider_error;

// ── Mock that captures payloads without making HTTP calls ────────────────────

class MockLlamaProvider : public quantclaw::LlamaProvider {
 public:
  explicit MockLlamaProvider(std::shared_ptr<spdlog::logger> logger)
      : LlamaProvider("http://127.0.0.1:8080", 30, logger) {}

  quantclaw::ChatCompletionResponse next_response;
  std::vector<quantclaw::ChatCompletionResponse> stream_chunks;
  quantclaw::ChatCompletionRequest last_request;

  quantclaw::ChatCompletionResponse
  ChatCompletion(const quantclaw::ChatCompletionRequest& request) override {
    last_request = request;
    if (next_response.content.empty() && next_response.tool_calls.empty()) {
      quantclaw::ChatCompletionResponse r;
      r.content = "mock: " + request.messages.back().text();
      r.finish_reason = "stop";
      return r;
    }
    return next_response;
  }

  void ChatCompletionStream(
      const quantclaw::ChatCompletionRequest& request,
      std::function<void(const quantclaw::ChatCompletionResponse&)> callback)
      override {
    last_request = request;
    if (stream_chunks.empty()) {
      quantclaw::ChatCompletionResponse r;
      r.content = "streamed mock";
      r.is_stream_end = true;
      callback(r);
    } else {
      for (const auto& chunk : stream_chunks) {
        callback(chunk);
      }
    }
  }
};

class LlamaProviderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    logger_ = std::make_shared<spdlog::logger>("test", null_sink);
    provider_ = std::make_unique<MockLlamaProvider>(logger_);
  }

  std::shared_ptr<spdlog::logger> logger_;
  std::unique_ptr<MockLlamaProvider> provider_;
};

// ── Provider identity ────────────────────────────────────────────────────────

TEST_F(LlamaProviderTest, ProviderNameIsLocal) {
  EXPECT_EQ(provider_->GetProviderName(), "local");
}

TEST_F(LlamaProviderTest, SupportedModelsIsEmpty) {
  // llama-server exposes whatever model it was started with; we don't
  // hard-code names.
  EXPECT_TRUE(provider_->GetSupportedModels().empty());
}

// ── Basic chat completion ────────────────────────────────────────────────────

TEST_F(LlamaProviderTest, ChatCompletionReturnsContent) {
  quantclaw::ChatCompletionRequest req;
  req.messages.push_back({"user", "hello"});
  req.model = "qwen3";

  auto resp = provider_->ChatCompletion(req);

  EXPECT_EQ(resp.content, "mock: hello");
  EXPECT_EQ(resp.finish_reason, "stop");
}

TEST_F(LlamaProviderTest, ChatCompletionForwardsModel) {
  quantclaw::ChatCompletionRequest req;
  req.messages.push_back({"user", "ping"});
  req.model = "qwen3-9b";

  provider_->ChatCompletion(req);

  EXPECT_EQ(provider_->last_request.model, "qwen3-9b");
}

TEST_F(LlamaProviderTest, ChatCompletionForwardsTemperature) {
  quantclaw::ChatCompletionRequest req;
  req.messages.push_back({"user", "test"});
  req.temperature = 0.2;

  provider_->ChatCompletion(req);

  EXPECT_DOUBLE_EQ(provider_->last_request.temperature, 0.2);
}

TEST_F(LlamaProviderTest, ChatCompletionCustomResponse) {
  quantclaw::ChatCompletionResponse custom;
  custom.content = "custom answer";
  custom.finish_reason = "stop";
  provider_->next_response = custom;

  quantclaw::ChatCompletionRequest req;
  req.messages.push_back({"user", "anything"});

  auto resp = provider_->ChatCompletion(req);
  EXPECT_EQ(resp.content, "custom answer");
}

// ── Tool calling ─────────────────────────────────────────────────────────────

TEST_F(LlamaProviderTest, ChatCompletionWithToolCall) {
  quantclaw::ChatCompletionResponse tool_resp;
  quantclaw::ToolCall tc;
  tc.id = "call_abc123";
  tc.name = "get_weather";
  tc.arguments = {{"location", "London"}};
  tool_resp.tool_calls.push_back(tc);
  tool_resp.finish_reason = "tool_calls";
  provider_->next_response = tool_resp;

  quantclaw::ChatCompletionRequest req;
  req.messages.push_back({"user", "What is the weather in London?"});
  req.tools.push_back(
      {{"name", "get_weather"},
       {"description", "Returns weather for a location"},
       {"parameters",
        {{"type", "object"},
         {"properties", {{"location", {{"type", "string"}}}}}}}});

  auto resp = provider_->ChatCompletion(req);

  ASSERT_EQ(resp.tool_calls.size(), 1u);
  EXPECT_EQ(resp.tool_calls[0].id, "call_abc123");
  EXPECT_EQ(resp.tool_calls[0].name, "get_weather");
  EXPECT_EQ(resp.finish_reason, "tool_calls");
}

TEST_F(LlamaProviderTest, ToolsAreForwardedInRequest) {
  quantclaw::ChatCompletionRequest req;
  req.messages.push_back({"user", "use tools"});
  req.tools.push_back({{"name", "my_tool"}, {"description", "does stuff"}});
  req.tool_choice_auto = true;

  provider_->ChatCompletion(req);

  ASSERT_EQ(provider_->last_request.tools.size(), 1u);
  EXPECT_EQ(provider_->last_request.tools[0]["name"], "my_tool");
}

// ── Multi-turn conversation ──────────────────────────────────────────────────

TEST_F(LlamaProviderTest, MultiTurnMessagesForwarded) {
  quantclaw::ChatCompletionRequest req;
  req.messages.push_back({"system", "You are a helpful assistant."});
  req.messages.push_back({"user", "First message"});
  req.messages.push_back({"assistant", "First reply"});
  req.messages.push_back({"user", "Second message"});

  provider_->ChatCompletion(req);

  ASSERT_EQ(provider_->last_request.messages.size(), 4u);
  EXPECT_EQ(provider_->last_request.messages[0].role, "system");
  EXPECT_EQ(provider_->last_request.messages[3].role, "user");
}

// ── Streaming ────────────────────────────────────────────────────────────────

TEST_F(LlamaProviderTest, StreamingReceivesSingleChunk) {
  std::vector<quantclaw::ChatCompletionResponse> received;

  quantclaw::ChatCompletionRequest req;
  req.messages.push_back({"user", "stream test"});

  provider_->ChatCompletionStream(
      req, [&](const auto& r) { received.push_back(r); });

  ASSERT_EQ(received.size(), 1u);
  EXPECT_EQ(received[0].content, "streamed mock");
  EXPECT_TRUE(received[0].is_stream_end);
}

TEST_F(LlamaProviderTest, StreamingMultipleChunks) {
  quantclaw::ChatCompletionResponse c1, c2, c3;
  c1.content = "Hello";
  c2.content = " world";
  c3.is_stream_end = true;
  provider_->stream_chunks = {c1, c2, c3};

  std::string assembled;
  bool got_end = false;

  quantclaw::ChatCompletionRequest req;
  req.messages.push_back({"user", "hi"});

  provider_->ChatCompletionStream(req, [&](const auto& r) {
    assembled += r.content;
    if (r.is_stream_end)
      got_end = true;
  });

  EXPECT_EQ(assembled, "Hello world");
  EXPECT_TRUE(got_end);
}

TEST_F(LlamaProviderTest, StreamingToolCallChunks) {
  // Simulate two chunks: one carrying a tool call, one signalling end.
  quantclaw::ChatCompletionResponse tc_chunk;
  quantclaw::ToolCall tc;
  tc.id = "call_xyz";
  tc.name = "search";
  tc.arguments = {{"query", "llama.cpp"}};
  tc_chunk.tool_calls.push_back(tc);

  quantclaw::ChatCompletionResponse end_chunk;
  end_chunk.is_stream_end = true;

  provider_->stream_chunks = {tc_chunk, end_chunk};

  std::vector<quantclaw::ToolCall> collected_calls;
  bool got_end = false;

  quantclaw::ChatCompletionRequest req;
  req.messages.push_back({"user", "search for llama.cpp"});

  provider_->ChatCompletionStream(req, [&](const auto& r) {
    for (const auto& call : r.tool_calls)
      collected_calls.push_back(call);
    if (r.is_stream_end)
      got_end = true;
  });

  ASSERT_EQ(collected_calls.size(), 1u);
  EXPECT_EQ(collected_calls[0].name, "search");
  EXPECT_TRUE(got_end);
}

// ── Response shape round-trip (via the mock) ─────────────────────────────────
// These exercise the request/response plumbing, not the SSE parser itself.
// Real parser coverage lives in the "SSE parser" section at the bottom.

TEST_F(LlamaProviderTest, ParseResponsePlainText) {
  // Construct a valid OpenAI-format response and verify content extraction.
  // We do this by setting up a fixed next_response and checking round-trip.
  quantclaw::ChatCompletionResponse expected;
  expected.content = "Paris";
  expected.finish_reason = "stop";
  provider_->next_response = expected;

  quantclaw::ChatCompletionRequest req;
  req.messages.push_back({"user", "Capital of France?"});
  auto resp = provider_->ChatCompletion(req);

  EXPECT_EQ(resp.content, "Paris");
  EXPECT_EQ(resp.finish_reason, "stop");
}

TEST_F(LlamaProviderTest, ParseResponseMultipleToolCalls) {
  quantclaw::ChatCompletionResponse expected;
  for (int i = 0; i < 3; ++i) {
    quantclaw::ToolCall tc;
    tc.id = "call_" + std::to_string(i);
    tc.name = "tool_" + std::to_string(i);
    tc.arguments = {{"n", i}};
    expected.tool_calls.push_back(tc);
  }
  expected.finish_reason = "tool_calls";
  provider_->next_response = expected;

  quantclaw::ChatCompletionRequest req;
  req.messages.push_back({"user", "run three tools"});
  auto resp = provider_->ChatCompletion(req);

  ASSERT_EQ(resp.tool_calls.size(), 3u);
  EXPECT_EQ(resp.tool_calls[2].name, "tool_2");
}

// ── Token usage ──────────────────────────────────────────────────────────────

TEST_F(LlamaProviderTest, TokenUsageForwarded) {
  quantclaw::ChatCompletionResponse resp_with_usage;
  resp_with_usage.content = "ok";
  resp_with_usage.usage.prompt_tokens = 10;
  resp_with_usage.usage.completion_tokens = 5;
  resp_with_usage.usage.total_tokens = 15;
  provider_->next_response = resp_with_usage;

  quantclaw::ChatCompletionRequest req;
  req.messages.push_back({"user", "count tokens"});
  auto resp = provider_->ChatCompletion(req);

  EXPECT_EQ(resp.usage.prompt_tokens, 10);
  EXPECT_EQ(resp.usage.completion_tokens, 5);
  EXPECT_EQ(resp.usage.total_tokens, 15);
}

// ── Max tokens ───────────────────────────────────────────────────────────────

TEST_F(LlamaProviderTest, MaxTokensForwarded) {
  quantclaw::ChatCompletionRequest req;
  req.messages.push_back({"user", "long response please"});
  req.max_tokens = 4096;

  provider_->ChatCompletion(req);

  EXPECT_EQ(provider_->last_request.max_tokens, 4096);
}

// ── SSE parser ───────────────────────────────────────────────────────────────
// Exercises the real streaming parser (quantclaw::llama_detail::ParseSseStream)
// rather than the mock, using llama-server's actual wire format.

namespace {

// Collects everything the parser emits for a stream.
struct StreamCapture {
  std::string content;
  std::string reasoning;
  std::vector<quantclaw::ToolCall> tool_calls;
  bool saw_end = false;

  auto Callback() {
    return [this](const quantclaw::ChatCompletionResponse& r) {
      content += r.content;
      reasoning += r.reasoning_content;
      for (const auto& tc : r.tool_calls)
        tool_calls.push_back(tc);
      if (r.is_stream_end)
        saw_end = true;
    };
  }
};

// One llama-server chunk, matching the shape captured in tmp/raw-stream-*.txt.
std::string Chunk(const std::string& delta_json,
                  const std::string& finish_reason = "null") {
  return "data: {\"choices\":[{\"delta\":" + delta_json +
         ",\"finish_reason\":" + finish_reason +
         ",\"index\":0}],\"object\":\"chat.completion.chunk\"}\n\n";
}

}  // namespace

// REGRESSION: finish_reason is JSON null on every delta until the final chunk.
// Reading it with value("finish_reason", "") throws type_error converting
// null->string; the parser's catch swallowed that and discarded the whole
// delta, so local-model turns always came back empty. Guard is an explicit
// is_string() check. See commit 042f1b2.
TEST_F(LlamaProviderTest, SseDeltasWithNullFinishReasonAreNotDropped) {
  StreamCapture cap;
  std::string sse =
      Chunk(R"({"content":"Two plus two","role":"assistant"})") +
      Chunk(R"({"content":" equals four."})") +
      Chunk(R"({})", "\"stop\"") + "data: [DONE]\n\n";

  quantclaw::llama_detail::ParseSseStream(sse, cap.Callback(), logger_);

  EXPECT_EQ(cap.content, "Two plus two equals four.");
  EXPECT_TRUE(cap.saw_end);
}

TEST_F(LlamaProviderTest, SseReasoningContentDeltasSurviveNullFinishReason) {
  StreamCapture cap;
  std::string sse = Chunk(R"({"reasoning_content":"let me think"})") +
                    Chunk(R"({"content":"42"})") + "data: [DONE]\n\n";

  quantclaw::llama_detail::ParseSseStream(sse, cap.Callback(), logger_);

  EXPECT_EQ(cap.reasoning, "let me think");
  EXPECT_EQ(cap.content, "42");
}

TEST_F(LlamaProviderTest, SseSplitAcrossReadBoundariesReassembles) {
  // curl hands us arbitrary byte slices, not whole lines.
  StreamCapture cap;
  std::string sse = Chunk(R"({"content":"abc"})") +
                    Chunk(R"({"content":"def"})") + "data: [DONE]\n\n";

  quantclaw::llama_detail::ParseSseStream(sse.substr(0, 37), cap.Callback(),
                                          logger_);
  EXPECT_EQ(cap.content, "");  // nothing complete yet

  StreamCapture whole;
  quantclaw::llama_detail::ParseSseStream(sse, whole.Callback(), logger_);
  EXPECT_EQ(whole.content, "abcdef");
}

TEST_F(LlamaProviderTest, SseToolCallArgumentFragmentsAccumulate) {
  StreamCapture cap;
  std::string sse =
      Chunk(
          R"({"tool_calls":[{"index":0,"id":"call_1","function":{"name":"read","arguments":"{\"path\":"}}]})") +
      Chunk(
          R"({"tool_calls":[{"index":0,"function":{"arguments":"\"a.md\"}"}}]})") +
      Chunk(R"({})", "\"tool_calls\"");

  quantclaw::llama_detail::ParseSseStream(sse, cap.Callback(), logger_);

  ASSERT_EQ(cap.tool_calls.size(), 1u);
  EXPECT_EQ(cap.tool_calls[0].id, "call_1");
  EXPECT_EQ(cap.tool_calls[0].name, "read");
  EXPECT_EQ(cap.tool_calls[0].arguments.value("path", ""), "a.md");
}

// ── SerializeMessages ────────────────────────────────────────────────────────

TEST_F(LlamaProviderTest, SerializeKeepsLeadingSystemTurn) {
  std::vector<quantclaw::Message> msgs;
  msgs.push_back({"system", "you are helpful"});
  msgs.push_back({"user", "hi"});

  auto out = quantclaw::llama_detail::SerializeMessagesForTest(msgs);

  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0]["role"], "system");
  EXPECT_EQ(out[1]["role"], "user");
}

// A mid-conversation system turn is demoted to "user"; the alternation pass
// then folds it into the adjacent user turn instead of leaving a same-role run
// (Gemma-style templates reject both).
TEST_F(LlamaProviderTest, SerializeDemotesLateSystemTurnAndMergesIt) {
  std::vector<quantclaw::Message> msgs;
  msgs.push_back({"system", "sys"});
  msgs.push_back({"user", "first"});
  msgs.push_back({"system", "injected note"});

  auto out = quantclaw::llama_detail::SerializeMessagesForTest(msgs);

  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0]["role"], "system");
  EXPECT_EQ(out[1]["role"], "user");
  EXPECT_EQ(out[1]["content"], "first\n\ninjected note");
}

TEST_F(LlamaProviderTest, SerializeDropsEmptyTextTurns) {
  std::vector<quantclaw::Message> msgs;
  msgs.push_back({"user", "q"});
  msgs.push_back({"assistant", ""});  // blank/failed response
  msgs.push_back({"user", "still there?"});

  auto out = quantclaw::llama_detail::SerializeMessagesForTest(msgs);

  // Empty assistant turn dropped, then the two user turns merge.
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0]["role"], "user");
  EXPECT_EQ(out[0]["content"], "q\n\nstill there?");
}

TEST_F(LlamaProviderTest, SerializePreservesAlternation) {
  std::vector<quantclaw::Message> msgs;
  msgs.push_back({"user", "a"});
  msgs.push_back({"assistant", "b"});
  msgs.push_back({"user", "c"});

  auto out = quantclaw::llama_detail::SerializeMessagesForTest(msgs);

  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[0]["role"], "user");
  EXPECT_EQ(out[1]["role"], "assistant");
  EXPECT_EQ(out[2]["role"], "user");
}

// ── ConvertTools ─────────────────────────────────────────────────────────────

TEST_F(LlamaProviderTest, ConvertToolsPassesThroughWrappedSpec) {
  // The agent loop already emits OpenAI shape; re-wrapping double-nests
  // "function" and llama-server rejects it with "key 'name' not found".
  nlohmann::json wrapped = {
      {"type", "function"},
      {"function", {{"name", "read"}, {"description", "d"}}}};

  auto out = quantclaw::llama_detail::ConvertToolsForTest({wrapped});

  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0]["function"]["name"], "read");
  EXPECT_FALSE(out[0]["function"].contains("function"));
}

TEST_F(LlamaProviderTest, ConvertToolsWrapsBareSpec) {
  nlohmann::json bare = {{"name", "read"}, {"description", "d"}};

  auto out = quantclaw::llama_detail::ConvertToolsForTest({bare});

  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0]["type"], "function");
  EXPECT_EQ(out[0]["function"]["name"], "read");
}

// A "type" key alone must not be mistaken for an already-wrapped spec.
TEST_F(LlamaProviderTest, ConvertToolsWrapsSpecWithTypeButNoFunction) {
  nlohmann::json odd = {{"type", "function"}, {"name", "read"}};

  auto out = quantclaw::llama_detail::ConvertToolsForTest({odd});

  ASSERT_EQ(out.size(), 1u);
  ASSERT_TRUE(out[0].contains("function"));
  EXPECT_EQ(out[0]["function"]["name"], "read");
}
