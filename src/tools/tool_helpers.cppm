// Copyright 2026 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0
//
// Small text/shell utilities shared by the tool implementations.
//
// These were file-static in tool_registry.cpp. Splitting that file into
// several implementation units made the sharing explicit rather than
// duplicating them per unit.

export module quantclaw.tools.tool_helpers;

import std;

export namespace quantclaw::tools {

// Percent-encode for use in a URL query component.
std::string url_encode(const std::string& s) {
  std::ostringstream out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out << c;
    } else {
      out << '%' << std::hex << std::uppercase << ((c >> 4) & 0xF) << (c & 0xF);
    }
  }
  return out.str();
}

// Strip HTML tags and decode basic entities → plain text.
std::string html_to_text(const std::string& html) {
  // Compiled once. std::regex construction is expensive and these were being
  // rebuilt on every web_fetch — five compilations per fetched page.
  static const std::regex kScriptStyle(
      "<(script|style)[^>]*>[\\s\\S]*?</(script|style)>", std::regex::icase);
  static const std::regex kTags("<[^>]+>");
  static const std::regex kSpaces("[ \t]+");
  static const std::regex kBlankRuns("\n{3,}");

  // Remove <script> and <style> blocks
  std::string text = std::regex_replace(html, kScriptStyle, " ");
  // Remove all remaining tags
  text = std::regex_replace(text, kTags, " ");
  // Decode basic entities
  auto replace_all = [](std::string s, const std::string& from,
                        const std::string& to) {
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
      s.replace(pos, from.size(), to);
      pos += to.size();
    }
    return s;
  };
  text = replace_all(text, "&amp;", "&");
  text = replace_all(text, "&lt;", "<");
  text = replace_all(text, "&gt;", ">");
  text = replace_all(text, "&quot;", "\"");
  text = replace_all(text, "&#39;", "'");
  text = replace_all(text, "&nbsp;", " ");
  // Collapse whitespace
  text = std::regex_replace(text, kSpaces, " ");
  text = std::regex_replace(text, kBlankRuns, "\n\n");
  return text;
}

// Short unique-ish id with a caller-supplied prefix.
std::string generate_id(const std::string& prefix = "bg") {
  thread_local static std::mt19937 rng(std::random_device{}());
  std::uniform_int_distribution<std::uint32_t> dist;
  std::ostringstream ss;
  ss << prefix << "_" << std::hex << dist(rng);
  return ss.str();
}

// POSIX shell single-quoting: wraps the value in single quotes and escapes
// embedded single quotes, so it is always passed as one literal argument.
std::string shell_quote(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  out += '\'';
  for (char c : s) {
    if (c == '\'')
      out += "'\\''";
    else
      out += c;
  }
  out += '\'';
  return out;
}

}  // namespace quantclaw::tools
