// Copyright 2026 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0
//
// Network and memory tools: web search, web fetch, and memory lookup.
//
// One of several implementation units of quantclaw.tools.tool_registry. The
// registry and its fifteen tool bodies lived in a single 2200-line file; the
// bodies are unchanged, only relocated.

module;

#include <httplib.h>
#include <spdlog/spdlog.h>

module quantclaw.tools.tool_registry;

import std;
import nlohmann.json;

import quantclaw.core.memory_search;
import quantclaw.tools.tool_helpers;

namespace fs = std::filesystem;

namespace quantclaw {

using namespace quantclaw::tools;

// ---------------------------------------------------------------------------
// web_search_tool — Cascade: Brave → Tavily → Perplexity → DuckDuckGo → Grok
// ---------------------------------------------------------------------------

std::string ToolRegistry::web_search_tool(const nlohmann::json& params) {
  std::string query = params.value("query", "");
  int count = std::clamp(params.value("count", 5), 1, 10);
  std::string freshness = params.value("freshness", "");
  if (query.empty())
    throw std::runtime_error("query is required");

  // Determine provider and API keys
  const char* brave_key = std::getenv("BRAVE_API_KEY");
  const char* tavily_key = std::getenv("TAVILY_API_KEY");
  const char* perp_key = std::getenv("PERPLEXITY_API_KEY");
  const char* xai_key = std::getenv("XAI_API_KEY");

  std::string last_error;

  // --- Brave Search ---
  if (brave_key && *brave_key) {
    try {
      std::string path = "/res/v1/web/search?q=" + url_encode(query) +
                         "&count=" + std::to_string(count);
      if (!freshness.empty())
        path += "&freshness=" + freshness;

      httplib::SSLClient cli("api.search.brave.com");
      cli.set_default_headers({{"Accept", "application/json"},
                               {"Accept-Encoding", "identity"},
                               {"X-Subscription-Token", brave_key}});
      cli.set_connection_timeout(10);
      cli.set_read_timeout(15);

      auto res = cli.Get(path);
      if (!res)
        throw std::runtime_error("Brave Search: connection failed");
      if (res->status != 200)
        throw std::runtime_error("Brave Search HTTP " +
                                 std::to_string(res->status));

      auto j = nlohmann::json::parse(res->body);
      nlohmann::json results = nlohmann::json::array();
      if (j.contains("web") && j["web"].contains("results")) {
        for (const auto& r : j["web"]["results"]) {
          nlohmann::json item;
          item["title"] = r.value("title", "");
          item["url"] = r.value("url", "");
          item["description"] = r.value("description", "");
          results.push_back(item);
        }
      }
      return nlohmann::json{
          {"provider", "brave"}, {"query", query}, {"results", results}}
          .dump();
    } catch (const std::exception& e) {
      last_error = std::string("Brave: ") + e.what();
    }
  }

  // --- Tavily Search ---
  if (tavily_key && *tavily_key) {
    try {
      nlohmann::json body = {{"api_key", tavily_key},
                             {"query", query},
                             {"max_results", count},
                             {"search_depth", "basic"}};
      std::string body_str = body.dump();

      httplib::SSLClient cli("api.tavily.com");
      cli.set_default_headers({{"Content-Type", "application/json"}});
      cli.set_connection_timeout(10);
      cli.set_read_timeout(15);

      auto res = cli.Post("/search", body_str, "application/json");
      if (!res)
        throw std::runtime_error("Tavily: connection failed");
      if (res->status != 200)
        throw std::runtime_error("Tavily HTTP " + std::to_string(res->status));

      auto j = nlohmann::json::parse(res->body);
      nlohmann::json results = nlohmann::json::array();
      if (j.contains("results")) {
        for (const auto& r : j["results"]) {
          nlohmann::json item;
          item["title"] = r.value("title", "");
          item["url"] = r.value("url", "");
          item["description"] = r.value("content", "");
          results.push_back(item);
        }
      }
      return nlohmann::json{
          {"provider", "tavily"}, {"query", query}, {"results", results}}
          .dump();
    } catch (const std::exception& e) {
      last_error = std::string("Tavily: ") + e.what();
    }
  }

  // --- Perplexity Sonar (OpenAI-compatible) ---
  if (perp_key && *perp_key) {
    try {
      nlohmann::json body = {
          {"model", "perplexity/sonar"},
          {"messages", nlohmann::json::array({nlohmann::json{
                           {"role", "user"}, {"content", query}}})},
          {"max_tokens", 1024}};
      std::string body_str = body.dump();

      httplib::SSLClient cli("api.perplexity.ai");
      cli.set_default_headers(
          {{"Authorization", std::string("Bearer ") + perp_key},
           {"Content-Type", "application/json"}});
      cli.set_connection_timeout(15);
      cli.set_read_timeout(30);

      auto res = cli.Post("/chat/completions", body_str, "application/json");
      if (!res)
        throw std::runtime_error("Perplexity: connection failed");
      if (res->status != 200)
        throw std::runtime_error("Perplexity HTTP " +
                                 std::to_string(res->status));

      auto j = nlohmann::json::parse(res->body);
      std::string answer;
      if (j.contains("choices") && !j["choices"].empty()) {
        answer = j["choices"][0]["message"].value("content", "");
      }
      nlohmann::json results = nlohmann::json::array();
      nlohmann::json perp_item;
      perp_item["title"] = "Perplexity Answer";
      perp_item["url"] = "";
      perp_item["description"] = answer;
      results.push_back(perp_item);
      return nlohmann::json{
          {"provider", "perplexity"}, {"query", query}, {"results", results}}
          .dump();
    } catch (const std::exception& e) {
      last_error = std::string("Perplexity: ") + e.what();
    }
  }

  // --- DuckDuckGo HTML scraping (no API key needed) ---
  try {
    std::string ddg_path = "/html/?q=" + url_encode(query);

    httplib::SSLClient cli("html.duckduckgo.com");
    cli.set_default_headers(
        {{"User-Agent", "QuantClaw/1.0"}, {"Accept", "text/html"}});
    cli.set_connection_timeout(10);
    cli.set_read_timeout(15);

    auto res = cli.Get(ddg_path);
    if (res && res->status == 200 && !res->body.empty()) {
      nlohmann::json results = nlohmann::json::array();
      // Extract result links: <a rel="nofollow" class="result__a"
      // href="URL">TITLE</a>
      std::regex link_re(
          R"REGEX(<a\s+[^>]*class="result__a"[^>]*href="([^"]*)"[^>]*>([\s\S]*?)</a>)REGEX",
          std::regex::icase);
      // Extract snippets: <a class="result__snippet" ...>DESCRIPTION</a>
      std::regex snippet_re(
          R"REGEX(<a\s+[^>]*class="result__snippet"[^>]*>([\s\S]*?)</a>)REGEX",
          std::regex::icase);
      std::vector<std::pair<std::string, std::string>> links;  // url, title
      std::vector<std::string> snippets;

      auto link_begin =
          std::sregex_iterator(res->body.begin(), res->body.end(), link_re);
      auto link_end = std::sregex_iterator();
      for (auto it = link_begin; it != link_end; ++it) {
        std::string url = (*it)[1].str();
        std::string title = html_to_text((*it)[2].str());
        // DuckDuckGo wraps URLs in a redirect; extract actual URL if present
        if (url.find("uddg=") != std::string::npos) {
          size_t uddg = url.find("uddg=") + 5;
          size_t amp = url.find('&', uddg);
          std::string encoded = (amp != std::string::npos)
                                    ? url.substr(uddg, amp - uddg)
                                    : url.substr(uddg);
          // Simple URL-decode for %XX
          std::ostringstream decoded;
          size_t i = 0;
          while (i < encoded.size()) {
            if (encoded[i] == '%' && i + 2 < encoded.size()) {
              unsigned int hi = 0;
              if (std::sscanf(encoded.substr(i + 1, 2).c_str(), "%x", &hi) ==
                  1) {
                decoded << static_cast<char>(hi);
                i += 3;  // skip '%' and its two hex digits
                continue;
              } else {
                decoded << encoded[i];
                ++i;
              }
            } else if (encoded[i] == '+') {
              decoded << ' ';
              ++i;
            } else {
              decoded << encoded[i];
              ++i;
            }
          }
          url = decoded.str();
        }
        links.emplace_back(url, title);
      }

      auto snip_begin =
          std::sregex_iterator(res->body.begin(), res->body.end(), snippet_re);
      auto snip_end = std::sregex_iterator();
      for (auto it = snip_begin; it != snip_end; ++it) {
        snippets.push_back(html_to_text((*it)[1].str()));
      }

      int n = std::min(count, static_cast<int>(links.size()));
      for (int i = 0; i < n; ++i) {
        nlohmann::json item;
        item["title"] = links[i].second;
        item["url"] = links[i].first;
        item["description"] =
            (i < static_cast<int>(snippets.size())) ? snippets[i] : "";
        results.push_back(item);
      }
      if (!results.empty()) {
        return nlohmann::json{
            {"provider", "duckduckgo"}, {"query", query}, {"results", results}}
            .dump();
      }
    }
  } catch (const std::exception& e) {
    last_error = std::string("DuckDuckGo: ") + e.what();
  }

  // --- xAI Grok ---
  if (xai_key && *xai_key) {
    try {
      nlohmann::json body = {
          {"model", "grok-3-mini"},
          {"messages", nlohmann::json::array({nlohmann::json{
                           {"role", "user"}, {"content", query}}})},
          {"max_tokens", 1024}};
      std::string body_str = body.dump();

      httplib::SSLClient cli("api.x.ai");
      cli.set_default_headers(
          {{"Authorization", std::string("Bearer ") + xai_key},
           {"Content-Type", "application/json"}});
      cli.set_connection_timeout(10);
      cli.set_read_timeout(30);

      auto res = cli.Post("/v1/chat/completions", body_str, "application/json");
      if (!res)
        throw std::runtime_error("xAI: connection failed");
      if (res->status != 200)
        throw std::runtime_error("xAI HTTP " + std::to_string(res->status));

      auto j = nlohmann::json::parse(res->body);
      std::string answer;
      if (j.contains("choices") && !j["choices"].empty()) {
        answer = j["choices"][0]["message"].value("content", "");
      }
      nlohmann::json results = nlohmann::json::array();
      nlohmann::json grok_item;
      grok_item["title"] = "Grok Answer";
      grok_item["url"] = "";
      grok_item["description"] = answer;
      results.push_back(grok_item);
      return nlohmann::json{
          {"provider", "grok"}, {"query", query}, {"results", results}}
          .dump();
    } catch (const std::exception& e) {
      last_error = std::string("Grok: ") + e.what();
    }
  }

  // All providers failed
  std::string error_msg = "web_search: no provider succeeded.";
  if (!last_error.empty()) {
    error_msg += " Last error: " + last_error;
  }
  error_msg +=
      " Configure BRAVE_API_KEY, TAVILY_API_KEY, PERPLEXITY_API_KEY, or "
      "XAI_API_KEY. DuckDuckGo requires no key.";
  throw std::runtime_error(error_msg);
}

// ---------------------------------------------------------------------------
// web_fetch_tool — HTTP GET + HTML-to-text
// ---------------------------------------------------------------------------

std::string ToolRegistry::web_fetch_tool(const nlohmann::json& params) {
  std::string url_str = params.value("url", "");
  int max_chars = params.value("maxChars", 50000);
  if (url_str.empty())
    throw std::runtime_error("url is required");

  // Parse URL — find scheme and host
  bool is_https = url_str.substr(0, 8) == "https://";
  bool is_http = url_str.substr(0, 7) == "http://";
  if (!is_https && !is_http)
    throw std::runtime_error("Only http:// and https:// URLs are supported");

  std::string stripped = is_https ? url_str.substr(8) : url_str.substr(7);
  size_t slash_pos = stripped.find('/');
  std::string host = (slash_pos != std::string::npos)
                         ? stripped.substr(0, slash_pos)
                         : stripped;
  std::string path =
      (slash_pos != std::string::npos) ? stripped.substr(slash_pos) : "/";

  // SSRF guard — block private ranges
  const std::vector<std::string> kBlocked = {
      "localhost", "127.", "0.0.0.0", "::1", "10.", "192.168.", "169.254."};
  for (const auto& b : kBlocked) {
    if (host.find(b) != std::string::npos) {
      throw std::runtime_error("SSRF guard: blocked host " + host);
    }
  }

  httplib::Headers headers = {
      {"User-Agent",
       "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
       "AppleWebKit/537.36 (KHTML, like Gecko) "
       "Chrome/122.0.0.0 Safari/537.36"},
      {"Accept",
       "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"},
      {"Accept-Language", "en-US,en;q=0.9"}};

  std::string body;
  std::string content_type;

  auto handle_response = [&](const httplib::Response& res) {
    body = res.body;
    content_type = res.get_header_value("Content-Type");
  };

  if (is_https) {
    httplib::SSLClient cli(host);
    cli.set_connection_timeout(10);
    cli.set_read_timeout(20);
    cli.set_follow_location(true);
    auto res = cli.Get(path, headers);
    if (!res)
      throw std::runtime_error("web_fetch: connection to " + host + " failed");
    if (res->status >= 400) {
      throw std::runtime_error("web_fetch HTTP " + std::to_string(res->status));
    }
    handle_response(*res);
  } else {
    httplib::Client cli(host);
    cli.set_connection_timeout(10);
    cli.set_read_timeout(20);
    cli.set_follow_location(true);
    auto res = cli.Get(path, headers);
    if (!res)
      throw std::runtime_error("web_fetch: connection to " + host + " failed");
    if (res->status >= 400) {
      throw std::runtime_error("web_fetch HTTP " + std::to_string(res->status));
    }
    handle_response(*res);
  }

  // Convert content to text
  std::string text;
  if (content_type.find("html") != std::string::npos) {
    text = html_to_text(body);
  } else if (content_type.find("json") != std::string::npos) {
    try {
      auto j = nlohmann::json::parse(body);
      text = j.dump(2);
    } catch (...) {
      text = body;
    }
  } else {
    text = body;
  }

  // Truncate
  if (static_cast<int>(text.size()) > max_chars) {
    text = text.substr(0, max_chars) + "\n\n[truncated at " +
           std::to_string(max_chars) + " chars]";
  }

  return nlohmann::json{
      {"url", url_str}, {"content", text}, {"contentType", content_type}}
      .dump();
}

// ---------------------------------------------------------------------------
// memory_search_tool
// ---------------------------------------------------------------------------

std::string ToolRegistry::memory_search_tool(const nlohmann::json& params) {
  std::string query = params.value("query", "");
  int max_results = params.value("maxResults", 10);
  if (query.empty())
    throw std::runtime_error("query is required");

  const char* home = std::getenv("HOME");
  std::string home_str = home ? home : "/tmp";
  auto workspace =
      std::filesystem::path(home_str) / ".quantclaw/agents/main/workspace";

  MemorySearch search(logger_);
  search.IndexDirectory(workspace);
  auto results = search.Search(query, max_results);

  nlohmann::json arr = nlohmann::json::array();
  for (const auto& r : results) {
    nlohmann::json entry;
    entry["source"] = r.source;
    entry["content"] = r.content;
    entry["score"] = r.score;
    entry["lineNumber"] = r.line_number;
    arr.push_back(entry);
  }
  return nlohmann::json{
      {"results", arr}, {"count", arr.size()}, {"query", query}}
      .dump();
}

// ---------------------------------------------------------------------------
// memory_get_tool
// ---------------------------------------------------------------------------

std::string ToolRegistry::memory_get_tool(const nlohmann::json& params) {
  std::string rel_path = params.value("path", "");
  if (rel_path.empty())
    throw std::runtime_error("path is required");

  const char* home = std::getenv("HOME");
  std::string home_str = home ? home : "/tmp";
  auto workspace =
      std::filesystem::path(home_str) / ".quantclaw/agents/main/workspace";
  auto full_path = workspace / rel_path;

  // Security: must remain inside workspace
  auto canonical = std::filesystem::weakly_canonical(full_path);
  auto ws_canon = std::filesystem::weakly_canonical(workspace);
  if (canonical.string().substr(0, ws_canon.string().size()) !=
      ws_canon.string()) {
    throw std::runtime_error("Access denied: path outside workspace");
  }

  if (!std::filesystem::exists(full_path)) {
    throw std::runtime_error("File not found: " + rel_path);
  }

  std::ifstream f(full_path);
  if (!f)
    throw std::runtime_error("Cannot read: " + rel_path);
  std::string content(std::istreambuf_iterator<char>(f), {});
  return nlohmann::json{{"path", rel_path}, {"content", content}}.dump();
}
}  // namespace quantclaw
