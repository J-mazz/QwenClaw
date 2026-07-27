// Copyright 2026 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0
//
// Recon-mode tools: subdomain enumeration, port and header scanning, DNS,
// WHOIS, certificate transparency, wayback, nuclei, and screenshots.
//
// One of several implementation units of quantclaw.tools.tool_registry. The
// registry and its fifteen tool bodies lived in a single 2200-line file; the
// bodies are unchanged, only relocated.

module;

#include <spdlog/spdlog.h>
#include <unistd.h>

module quantclaw.tools.tool_registry;

import std;
import nlohmann.json;

import quantclaw.core.recon_runtime;
import quantclaw.platform.process;
import quantclaw.security.scope_validator;
import quantclaw.tools.tool_helpers;

namespace fs = std::filesystem;

namespace quantclaw {

using namespace quantclaw::tools;

// ---------------------------------------------------------------------------
// RegisterReconTools — bug bounty reconnaissance tools
// ---------------------------------------------------------------------------

void ToolRegistry::RegisterReconTools() {
  logger_->info("Registering recon tools");

  // Helper: run a command and return {output, exit_code} JSON.
  auto run_recon_cmd = [this](const std::string& cmd,
                              int timeout = 120) -> nlohmann::json {
    auto result = platform::exec_capture(cmd, timeout);
    return {{"output", result.output}, {"exit_code", result.exit_code}};
  };

  // Helper: check if a binary is available on PATH.
  // Scans $PATH in-process rather than shelling out to `which`: a subprocess
  // here would inherit exec_capture's resource limits and, on a busy user
  // account, can fail to fork — making every check stall its full timeout.
  auto has_binary = [](const std::string& name) -> bool {
    const char* path_env = std::getenv("PATH");
    if (!path_env) {
      return false;
    }
    std::string_view path(path_env);
    for (std::size_t start = 0; start <= path.size();) {
      std::size_t end = path.find(':', start);
      std::string_view dir =
          path.substr(start, end == std::string_view::npos ? std::string_view::npos
                                                           : end - start);
      if (!dir.empty()) {
        std::error_code ec;
        fs::path candidate = fs::path(dir) / name;
        if (fs::is_regular_file(candidate, ec) &&
            ::access(candidate.c_str(), X_OK) == 0) {
          return true;
        }
      }
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }
    return false;
  };

  // --- 1. subdomain_enum ---
  if (has_binary("subfinder") || has_binary("amass")) {
    register_tool(
        "subdomain_enum",
        "Enumerate subdomains for a target domain using subfinder or amass.",
        nlohmann::json{
            {"type", "object"},
            {"properties",
             {{"target",
               {{"type", "string"},
                {"description", "Target domain (e.g., example.com)"}}},
              {"timeout",
               {{"type", "integer"},
                {"description", "Timeout in seconds (default 120)"}}}}},
            {"required", {"target"}}},
        [this, run_recon_cmd, has_binary](
            const nlohmann::json& params) -> std::string {
          auto target = params["target"].get<std::string>();
          int timeout = params.value("timeout", 120);
          std::string cmd;
          if (has_binary("subfinder")) {
            cmd = "subfinder -d " + shell_quote(target) + " -silent 2>/dev/null";
          } else {
            cmd = "amass enum -passive -d " + shell_quote(target) +
                  " 2>/dev/null";
          }
          auto result = run_recon_cmd(cmd, timeout);
          return result.dump();
        });
  }

  // --- 2. port_scan ---
  if (has_binary("naabu") || has_binary("nmap")) {
    register_tool(
        "port_scan",
        "Scan open ports on a target host using naabu or nmap.",
        nlohmann::json{
            {"type", "object"},
            {"properties",
             {{"target",
               {{"type", "string"},
                {"description", "Target host or IP"}}},
              {"ports",
               {{"type", "string"},
                {"description",
                 "Port specification (e.g., '80,443', '1-1000', default "
                 "top-100)"}}},
              {"timeout",
               {{"type", "integer"},
                {"description", "Timeout in seconds (default 120)"}}}}},
            {"required", {"target"}}},
        [this, run_recon_cmd, has_binary](
            const nlohmann::json& params) -> std::string {
          auto target = params["target"].get<std::string>();
          auto ports = params.value("ports", "");
          int timeout = params.value("timeout", 120);
          std::string cmd;
          if (has_binary("naabu")) {
            cmd = "naabu -host " + shell_quote(target) + " -json -silent";
            if (!ports.empty())
              cmd += " -p " + shell_quote(ports);
          } else {
            cmd = "nmap -sT -T4 --open -oX -";
            if (!ports.empty())
              cmd += " -p " + shell_quote(ports);
            cmd += " " + shell_quote(target);
          }
          cmd += " 2>/dev/null";
          auto result = run_recon_cmd(cmd, timeout);
          return result.dump();
        });
  }

  // --- 3. header_analysis ---
  if (has_binary("httpx") || has_binary("curl")) {
    register_tool(
        "header_analysis",
        "Analyze HTTP headers for security issues (HSTS, CSP, etc.).",
        nlohmann::json{
            {"type", "object"},
            {"properties",
             {{"target",
               {{"type", "string"},
                {"description", "Target URL or domain"}}},
              {"follow_redirects",
               {{"type", "boolean"},
                {"description", "Follow redirects (default true)"}}}}},
            {"required", {"target"}}},
        [this, run_recon_cmd, has_binary](
            const nlohmann::json& params) -> std::string {
          auto target = params["target"].get<std::string>();
          bool follow = params.value("follow_redirects", true);
          std::string cmd;
          if (has_binary("httpx")) {
            cmd = "echo " + shell_quote(target) +
                  " | httpx -json -silent -title -tech-detect -status-code "
                  "-content-length";
            if (follow) cmd += " -follow-redirects";
          } else {
            cmd = "curl -sI";
            if (follow) cmd += " -L";
            cmd += " " + shell_quote(target);
          }
          cmd += " 2>/dev/null";
          auto result = run_recon_cmd(cmd, 30);
          return result.dump();
        });
  }

  // --- 4. dns_lookup ---
  if (has_binary("dig")) {
    register_tool(
        "dns_lookup",
        "Perform DNS lookups for a target domain.",
        nlohmann::json{
            {"type", "object"},
            {"properties",
             {{"target",
               {{"type", "string"},
                {"description", "Target domain"}}},
              {"record_type",
               {{"type", "string"},
                {"description",
                 "DNS record type (A, AAAA, MX, NS, TXT, CNAME, ANY). "
                 "Default: A"}}},
              {"nameserver",
               {{"type", "string"},
                {"description",
                 "Nameserver to query (default: system resolver)"}}}}},
            {"required", {"target"}}},
        [this, run_recon_cmd](const nlohmann::json& params) -> std::string {
          auto target = params["target"].get<std::string>();
          auto rtype = params.value("record_type", "A");
          auto ns = params.value("nameserver", "");
          std::string cmd =
              "dig " + shell_quote(target) + " " + shell_quote(rtype) +
              " +short";
          if (!ns.empty())
            cmd += " " + shell_quote("@" + ns);
          cmd += " 2>/dev/null";
          auto result = run_recon_cmd(cmd, 15);
          return result.dump();
        });
  }

  // --- 5. whois_lookup ---
  if (has_binary("whois")) {
    register_tool(
        "whois_lookup",
        "WHOIS lookup for domain registration data.",
        nlohmann::json{
            {"type", "object"},
            {"properties",
             {{"target",
               {{"type", "string"},
                {"description", "Domain or IP to look up"}}}}},
            {"required", {"target"}}},
        [this, run_recon_cmd](const nlohmann::json& params) -> std::string {
          auto target = params["target"].get<std::string>();
          std::string cmd = "whois " + shell_quote(target) + " 2>/dev/null";
          auto result = run_recon_cmd(cmd, 30);
          return result.dump();
        });
  }

  // --- 6. cert_transparency ---
  register_tool(
      "cert_transparency",
      "Search Certificate Transparency logs (crt.sh) for subdomains.",
      nlohmann::json{
          {"type", "object"},
          {"properties",
           {{"target",
             {{"type", "string"},
              {"description", "Domain to search CT logs for"}}},
            {"include_expired",
             {{"type", "boolean"},
              {"description", "Include expired certs (default false)"}}}}},
          {"required", {"target"}}},
      [this, run_recon_cmd](const nlohmann::json& params) -> std::string {
        auto target = params["target"].get<std::string>();
        bool expired = params.value("include_expired", false);
        std::string url =
            "https://crt.sh/?q=%25." + url_encode(target) + "&output=json";
        if (!expired) url += "&exclude=expired";
        std::string cmd =
            "curl -s " + shell_quote(url) + " 2>/dev/null | head -c 65536";
        auto result = run_recon_cmd(cmd, 30);
        return result.dump();
      });

  // --- 7. wayback_fetch ---
  if (has_binary("waybackurls") || has_binary("curl")) {
    register_tool(
        "wayback_fetch",
        "Fetch historical URLs from the Wayback Machine.",
        nlohmann::json{
            {"type", "object"},
            {"properties",
             {{"target",
               {{"type", "string"},
                {"description", "Domain to query Wayback Machine for"}}},
              {"filter",
               {{"type", "string"},
                {"description",
                 "Grep filter pattern for URLs (optional)"}}}}},
            {"required", {"target"}}},
        [this, run_recon_cmd, has_binary](
            const nlohmann::json& params) -> std::string {
          auto target = params["target"].get<std::string>();
          auto filter = params.value("filter", "");
          std::string cmd;
          if (has_binary("waybackurls")) {
            cmd = "echo " + shell_quote(target) + " | waybackurls 2>/dev/null";
          } else {
            cmd = "curl -s " +
                  shell_quote("https://web.archive.org/cdx/search/cdx?url=*." +
                              url_encode(target) +
                              "&output=text&fl=original&collapse=urlkey") +
                  " 2>/dev/null";
          }
          if (!filter.empty()) {
            cmd += " | grep -i " + shell_quote(filter);
          }
          cmd += " | sort -u | head -500";
          auto result = run_recon_cmd(cmd, 60);
          return result.dump();
        });
  }

  // --- 8. nuclei_scan ---
  if (has_binary("nuclei")) {
    register_tool(
        "nuclei_scan",
        "Run Nuclei vulnerability scanner with template-based detection.",
        nlohmann::json{
            {"type", "object"},
            {"properties",
             {{"target",
               {{"type", "string"},
                {"description", "Target URL or host"}}},
              {"templates",
               {{"type", "array"},
                {"items", {{"type", "string"}}},
                {"description",
                 "Specific template IDs to run (optional)"}}},
              {"severity",
               {{"type", "string"},
                {"description",
                 "Severity filter: info, low, medium, high (default: "
                 "medium,high)"}}},
              {"timeout",
               {{"type", "integer"},
                {"description", "Timeout in seconds (default 300)"}}}}},
            {"required", {"target"}}},
        [this, run_recon_cmd](const nlohmann::json& params) -> std::string {
          auto target = params["target"].get<std::string>();
          auto severity = params.value("severity", "medium,high");
          int timeout = params.value("timeout", 300);
          std::string cmd =
              "nuclei -u " + shell_quote(target) + " -json -silent";
          cmd += " -severity " + shell_quote(severity);
          if (params.contains("templates") && params["templates"].is_array()) {
            for (const auto& t : params["templates"]) {
              if (t.is_string())
                cmd += " -t " + shell_quote(t.get<std::string>());
            }
          }
          cmd += " 2>/dev/null";
          auto result = run_recon_cmd(cmd, timeout);
          return result.dump();
        });
  }

  // --- 9. screenshot ---
  if (has_binary("gowitness") || has_binary("chromium") ||
      has_binary("chromium-browser")) {
    register_tool(
        "screenshot",
        "Take a screenshot of a web page for visual fingerprinting.",
        nlohmann::json{
            {"type", "object"},
            {"properties",
             {{"target",
               {{"type", "string"},
                {"description", "Target URL to screenshot"}}},
              {"full_page",
               {{"type", "boolean"},
                {"description", "Capture full page (default false)"}}}}},
            {"required", {"target"}}},
        [this, run_recon_cmd, has_binary](
            const nlohmann::json& params) -> std::string {
          auto target = params["target"].get<std::string>();
          std::string cmd;
          if (has_binary("gowitness")) {
            cmd = "gowitness single " + shell_quote(target) +
                  " --screenshot-path /tmp/recon-screenshots/ "
                  "--screenshot-format png 2>/dev/null";
          } else {
            std::string chrome =
                has_binary("chromium") ? "chromium" : "chromium-browser";
            cmd = chrome +
                  " --headless --disable-gpu --screenshot=/tmp/recon-"
                  "screenshots/screenshot.png --window-size=1280,1024 " +
                  shell_quote(target) + " 2>/dev/null";
          }
          auto result = run_recon_cmd(cmd, 30);
          result["screenshot_dir"] = "/tmp/recon-screenshots/";
          return result.dump();
        });
  }

  logger_->info("Recon tools registered");
}
}  // namespace quantclaw
