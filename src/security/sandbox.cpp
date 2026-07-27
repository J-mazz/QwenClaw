// Copyright 2025 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0

module;

#include <spdlog/spdlog.h>

module quantclaw.security.sandbox;

import std;

#ifdef __linux__
#include <sys/resource.h>
#endif

namespace quantclaw {

namespace {

// Is `candidate` inside `base`, comparing whole path components?
//
// The single containment rule for the whole sandbox. String prefix matching
// (`candidate.find(base) == 0`) is the classic wrong answer: it accepts
// "/tmp2/evil" as being inside "/tmp", and rejects nothing that merely shares
// a textual prefix. ValidateFilePath already iterated components; IsPathAllowed
// still used the string form, so the two disagreed about the same path.
bool path_within(const std::filesystem::path& base,
                 const std::filesystem::path& candidate) {
  auto b = base.begin();
  auto c = candidate.begin();
  for (; b != base.end(); ++b, ++c) {
    if (c == candidate.end()) {
      return false;  // candidate is shorter than base
    }
#ifdef _WIN32
    auto to_lower = [](std::string s) {
      std::transform(s.begin(), s.end(), s.begin(),
                     [](unsigned char ch) { return std::tolower(ch); });
      return s;
    };
    if (to_lower(b->string()) != to_lower(c->string())) {
      return false;
    }
#else
    if (*b != *c) {
      return false;
    }
#endif
  }
  return true;
}

}  // namespace

Sandbox::Sandbox(const std::filesystem::path& workspace_path,
                 const std::vector<std::string>& allowed_paths,
                 const std::vector<std::string>& denied_paths,
                 const std::vector<std::string>& allowed_commands,
                 const std::vector<std::string>& denied_commands)
    : workspace_path_(workspace_path),
      allowed_paths_(allowed_paths),
      denied_paths_(denied_paths),
      allowed_commands_(allowed_commands),
      denied_commands_(denied_commands) {
  for (const auto& cmd : denied_commands_) {
    // Patterns come from user config. An invalid one used to throw out of the
    // constructor, taking down whatever was constructing the sandbox; skip it
    // and keep the rest of the deny list in force.
    try {
      denied_cmd_patterns_.emplace_back(cmd, std::regex_constants::icase);
    } catch (const std::regex_error&) {
      // Intentionally ignored: a malformed deny pattern must not disable the
      // other patterns or abort startup.
    }
  }
}

bool Sandbox::IsPathAllowed(const std::string& path) const {
  std::filesystem::path resolved_path =
      std::filesystem::absolute(path).lexically_normal();

  // Check against denied paths first
  for (const auto& denied_path : denied_paths_) {
    if (path_within(std::filesystem::absolute(denied_path).lexically_normal(),
                    resolved_path)) {
      return false;
    }
  }

  // If allowed paths are specified, check against them
  if (!allowed_paths_.empty()) {
    for (const auto& allowed_path : allowed_paths_) {
      if (path_within(std::filesystem::absolute(allowed_path).lexically_normal(),
                      resolved_path)) {
        return true;
      }
    }
    return false;
  }

  return true;
}

bool Sandbox::IsCommandAllowed(const std::string& command) const {
  for (const auto& pattern : denied_cmd_patterns_) {
    if (std::regex_search(command, pattern)) {
      return false;
    }
  }
  return true;
}

std::string Sandbox::SanitizePath(const std::string& path) const {
  std::filesystem::path clean_path =
      std::filesystem::path(path).lexically_normal();
  if (clean_path.string().substr(0, 2) == "..") {
    throw std::runtime_error("Path traversal detected: " + path);
  }
  return clean_path.string();
}

bool Sandbox::ValidateFilePath(const std::string& path,
                               const std::string& workspace) {
  namespace fs = std::filesystem;
  std::error_code ec;

  // Resolve workspace and path to canonical form so symlinks are resolved
  // for the existing prefix and ".." segments are collapsed.
  fs::path ws_abs = fs::weakly_canonical(workspace, ec);
  if (ec)
    return false;

  // Resolve relative paths against the workspace root (not CWD).
  fs::path input_path(path);
  if (input_path.is_relative()) {
    input_path = ws_abs / input_path;
  }
  fs::path path_abs = fs::weakly_canonical(input_path, ec);
  if (ec)
    return false;

  // NOTE: there is deliberately no `path_str.find("..")` check here. After
  // weakly_canonical the ".." segments are already resolved, so it caught no
  // traversal — it only rejected legitimate names that happen to contain two
  // dots, e.g. "report..md" or "v1..2/notes.txt".
  return path_within(ws_abs, path_abs);
}

bool Sandbox::ValidateShellCommand(const std::string& command) {
  // Block obviously dangerous commands
  static const std::vector<std::regex> dangerous_patterns = {
      std::regex(R"(\brm\s+-rf\s+/)", std::regex_constants::icase),
      std::regex(R"(\bmkfs\b)", std::regex_constants::icase),
      std::regex(R"(\bdd\s+if=)", std::regex_constants::icase),
  };

  for (const auto& pattern : dangerous_patterns) {
    if (std::regex_search(command, pattern)) {
      return false;
    }
  }
  return true;
}

void Sandbox::ApplyResourceLimits() {
  // Resource limits are now applied inside exec_capture() on the child
  // process (via fork + setrlimit before exec on Linux). Calling setrlimit
  // on the host process would permanently cap the gateway itself.
  // This function is intentionally a no-op; the actual enforcement lives
  // in process_unix.cpp.
}

}  // namespace quantclaw
