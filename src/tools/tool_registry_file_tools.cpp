// Copyright 2026 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0
//
// File tools: path resolution, read/write/edit, and patch application.
//
// One of several implementation units of quantclaw.tools.tool_registry. The
// registry and its fifteen tool bodies lived in a single 2200-line file; the
// bodies are unchanged, only relocated.

module;

#include <spdlog/spdlog.h>

module quantclaw.tools.tool_registry;

import std;
import nlohmann.json;

import quantclaw.common.atomic_file;
import quantclaw.constants;
import quantclaw.security.sandbox;
import quantclaw.tools.tool_helpers;

namespace fs = std::filesystem;

namespace quantclaw {

using namespace quantclaw::tools;

// ---------------------------------------------------------------------------
// File tools
// ---------------------------------------------------------------------------

// Resolve a tool-supplied path against the agent workspace: relative paths are
// rooted at the workspace (which is also the sandbox root) so the agent can use
// simple names like "notes.md"; absolute paths are returned as-is for the
// sandbox check to validate.
std::string ToolRegistry::resolve_workspace_path(const std::string& path) const {
  std::filesystem::path p(path);
  if (p.is_relative() && !workspace_path_.empty()) {
    p = std::filesystem::path(workspace_path_) / p;
  }
  return p.lexically_normal().string();
}

std::string ToolRegistry::read_file_tool(const nlohmann::json& params) {
  if (!params.contains("path"))
    throw std::runtime_error("Missing required parameter: path");
  std::string path = resolve_workspace_path(params["path"].get<std::string>());
  if (!quantclaw::SecuritySandbox::ValidateFilePath(path, workspace_path_))
    throw std::runtime_error("Access denied: path outside workspace: " + path);
  if (!std::filesystem::exists(path))
    throw std::runtime_error("File not found: " + path);

  // Refuse rather than slurp: the whole file used to be read into memory with
  // no ceiling, so pointing the tool at a multi-gigabyte artifact took the
  // gateway down. The limit is well above any file worth sending to a model.
  std::error_code ec;
  auto size = std::filesystem::file_size(path, ec);
  if (!ec && size > kMaxReadFileBytes) {
    throw std::runtime_error(
        "File too large to read: " + path + " (" + std::to_string(size) +
        " bytes, limit " + std::to_string(kMaxReadFileBytes) +
        "). Use exec with head/sed to read a portion.");
  }

  std::ifstream f(path);
  if (!f)
    throw std::runtime_error("Failed to open: " + path);
  return std::string(std::istreambuf_iterator<char>(f), {});
}

std::string ToolRegistry::write_file_tool(const nlohmann::json& params) {
  if (!params.contains("path") || !params.contains("content"))
    throw std::runtime_error("Missing required parameters: path, content");
  std::string path = resolve_workspace_path(params["path"].get<std::string>());
  std::string content = params["content"].get<std::string>();
  if (!quantclaw::SecuritySandbox::ValidateFilePath(path, workspace_path_))
    throw std::runtime_error("Access denied: path outside workspace: " + path);
  // Atomic: an interrupted write previously left the file truncated or
  // half-written, destroying whatever was there before.
  std::string err;
  if (!WriteFileAtomically(path, content, &err))
    throw std::runtime_error("Failed to write: " + path + ": " + err);
  return "Successfully wrote to file: " + path;
}

std::string ToolRegistry::edit_file_tool(const nlohmann::json& params) {
  if (!params.contains("path") || !params.contains("oldText") ||
      !params.contains("newText"))
    throw std::runtime_error(
        "Missing required parameters: path, oldText, newText");
  // resolve_workspace_path, like read and write. Commit e4f6572 rooted those
  // two at the workspace but missed edit, so a relative path here resolved
  // against the gateway's own working directory instead — the same argument
  // meant different files depending on which tool the model picked.
  std::string path = resolve_workspace_path(params["path"].get<std::string>());
  std::string old_text = params["oldText"].get<std::string>();
  std::string new_text = params["newText"].get<std::string>();
  if (old_text.empty())
    throw std::runtime_error("oldText must not be empty");
  if (!quantclaw::SecuritySandbox::ValidateFilePath(path, workspace_path_))
    throw std::runtime_error("Access denied: path outside workspace: " + path);
  std::ifstream f(path);
  if (!f)
    throw std::runtime_error("Failed to open: " + path);
  std::string content(std::istreambuf_iterator<char>(f), {});

  size_t pos = content.find(old_text);
  if (pos == std::string::npos)
    throw std::runtime_error("Text not found in file: " + old_text);
  // Require a unique match. Replacing the first of several occurrences edits
  // an arbitrary one and reports success, which is worse than refusing: the
  // model cannot tell it changed the wrong line.
  if (content.find(old_text, pos + 1) != std::string::npos)
    throw std::runtime_error(
        "oldText is ambiguous: it appears more than once in " + path +
        ". Include surrounding context to make it unique.");

  content.replace(pos, old_text.size(), new_text);

  std::string err;
  if (!WriteFileAtomically(path, content, &err))
    throw std::runtime_error("Failed to write edited file: " + path + ": " +
                             err);
  return "Successfully edited file: " + path;
}


// ---------------------------------------------------------------------------
// apply_patch_tool
// Supports: *** Begin Patch / *** End Patch wrapper
//   *** Add File: <path>      → create file with content below
//   *** Update File: <path>   → apply unified diff hunks
//   *** Delete File: <path>   → remove file
// ---------------------------------------------------------------------------

std::string ToolRegistry::apply_patch_tool(const nlohmann::json& params) {
  if (!params.contains("patch"))
    throw std::runtime_error("patch is required");
  std::string patch = params["patch"].get<std::string>();

  // Find Begin/End markers
  const std::string kBegin = "*** Begin Patch";
  const std::string kEnd = "*** End Patch";
  size_t begin_pos = patch.find(kBegin);
  size_t end_pos = patch.find(kEnd);
  if (begin_pos == std::string::npos)
    throw std::runtime_error("Missing '*** Begin Patch' marker");
  std::string body = (end_pos != std::string::npos)
                         ? patch.substr(begin_pos + kBegin.size(),
                                        end_pos - begin_pos - kBegin.size())
                         : patch.substr(begin_pos + kBegin.size());

  // Split into lines
  std::vector<std::string> lines;
  std::istringstream iss(body);
  std::string line;
  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    lines.push_back(line);
  }

  int applied = 0;
  std::string current_file;
  enum class FileOp { kNone, kAdd, kUpdate, kDelete } op = FileOp::kNone;
  std::vector<std::string> add_content;
  std::vector<std::string> diff_hunks;

  auto flush_file = [&]() {
    if (current_file.empty())
      return;
    if (!quantclaw::SecuritySandbox::ValidateFilePath(current_file,
                                                      workspace_path_))
      throw std::runtime_error("Access denied: path outside workspace: " +
                               current_file);
    if (op == FileOp::kAdd) {
      std::filesystem::create_directories(
          std::filesystem::path(current_file).parent_path());
      std::ofstream f(current_file);
      for (const auto& l : add_content)
        f << l << "\n";
      ++applied;
    } else if (op == FileOp::kDelete) {
      if (std::filesystem::exists(current_file))
        std::filesystem::remove(current_file);
      ++applied;
    } else if (op == FileOp::kUpdate && !diff_hunks.empty()) {
      // Read current file content
      std::ifstream f(current_file);
      if (!f)
        throw std::runtime_error("Cannot open for update: " + current_file);
      std::vector<std::string> file_lines;
      std::string fl;
      while (std::getline(f, fl)) {
        if (!fl.empty() && fl.back() == '\r')
          fl.pop_back();
        file_lines.push_back(fl);
      }
      f.close();

      // Apply hunks (simple line-based application)
      // Each hunk starts with @@ -start,count +start,count @@
      std::vector<std::string> result_lines = file_lines;
      int line_offset = 0;
      size_t i = 0;
      while (i < diff_hunks.size()) {
        std::string& hl = diff_hunks[i];
        if (hl.size() >= 2 && hl.substr(0, 2) == "@@") {
          // Parse @@ -old_start,old_count +new_start,new_count @@
          int old_start = 0;
          int old_count = 0;
          sscanf(hl.c_str(), "@@ -%d,%d", &old_start, &old_count);
          int apply_at = old_start - 1 + line_offset;  // 0-indexed

          // Collect hunk lines
          std::vector<std::string> removed, added;
          size_t j = i + 1;
          while (j < diff_hunks.size() && diff_hunks[j].size() >= 2 &&
                 diff_hunks[j].substr(0, 2) != "@@") {
            char ch = diff_hunks[j][0];
            std::string content = diff_hunks[j].substr(1);
            if (ch == '-')
              removed.push_back(content);
            else if (ch == '+')
              added.push_back(content);
            ++j;
          }

          // Splice: replace removed lines with added lines
          if (apply_at >= 0 &&
              apply_at <= static_cast<int>(result_lines.size())) {
            result_lines.erase(result_lines.begin() + apply_at,
                               result_lines.begin() + apply_at +
                                   static_cast<int>(removed.size()));
            result_lines.insert(result_lines.begin() + apply_at, added.begin(),
                                added.end());
            line_offset += static_cast<int>(added.size()) -
                           static_cast<int>(removed.size());
          }
          i = j;
        } else {
          ++i;
        }
      }

      std::ofstream out(current_file);
      if (!out)
        throw std::runtime_error("Cannot write: " + current_file);
      for (const auto& l : result_lines)
        out << l << "\n";
      ++applied;
    }

    current_file.clear();
    op = FileOp::kNone;
    add_content.clear();
    diff_hunks.clear();
  };

  constexpr std::string_view kAddMarker = "*** Add File: ";
  constexpr std::string_view kUpdateMarker = "*** Update File: ";
  constexpr std::string_view kDeleteMarker = "*** Delete File: ";
  for (const auto& l : lines) {
    if (l.starts_with(kAddMarker)) {
      flush_file();
      current_file = l.substr(kAddMarker.size());
      op = FileOp::kAdd;
    } else if (l.starts_with(kUpdateMarker)) {
      flush_file();
      current_file = l.substr(kUpdateMarker.size());
      op = FileOp::kUpdate;
    } else if (l.starts_with(kDeleteMarker)) {
      flush_file();
      current_file = l.substr(kDeleteMarker.size());
      op = FileOp::kDelete;
    } else if (op == FileOp::kAdd) {
      add_content.push_back(l);
    } else if (op == FileOp::kUpdate) {
      diff_hunks.push_back(l);
    }
  }
  flush_file();

  return "Applied patch: " + std::to_string(applied) + " file(s) modified.";
}
}  // namespace quantclaw
