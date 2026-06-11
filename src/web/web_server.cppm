// Copyright 2025 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0

module;

#include <httplib.h>
#include <spdlog/spdlog.h>

export module quantclaw.web.web_server;

import std;
import nlohmann.json;

namespace quantclaw::web {

export class WebServer {
 public:
  using RequestHandler =
      std::function<std::string(const std::string&, const std::string&)>;
  using RawHandler =
      std::function<void(const httplib::Request&, httplib::Response&)>;

  WebServer(int port, std::shared_ptr<spdlog::logger> logger);
  ~WebServer();

  // Simplified route — handler receives (method, body) and returns response
  // string
  void AddRoute(const std::string& path, const std::string& method,
                RequestHandler handler);

  // Raw route — handler receives full httplib Request/Response (query params,
  // headers, etc.)
  void AddRawRoute(const std::string& path, const std::string& method,
                   RawHandler handler);

  // Enable CORS headers on all responses
  void EnableCors(const std::string& allowed_origin = "*");

  // Set bearer token for auth (empty = no auth check)
  void SetAuthToken(const std::string& token);

  // Mount a directory for static file serving
  void SetMountPoint(const std::string& mount, const std::string& dir);

  // Set the interface address to bind on (e.g. "127.0.0.1" for loopback-only,
  // "0.0.0.0" for all interfaces). Defaults to loopback for safety.
  void SetBindHost(const std::string& host) {
    bind_host_ = host;
  }

  void Start();
  void Stop();

 private:
  struct RouteInfo {
    std::string method;
    RequestHandler handler;
  };

  int port_;
  std::string bind_host_ = "127.0.0.1";
  std::shared_ptr<spdlog::logger> logger_;
  std::atomic<bool> running_;
  std::unique_ptr<std::thread> server_thread_;
  std::unordered_map<std::string, RouteInfo> routes_;
  std::vector<std::tuple<std::string, std::string, RawHandler>> raw_routes_;
  std::unique_ptr<httplib::Server> http_server_;

  bool cors_enabled_ = false;
  std::string cors_origin_;
  std::string auth_token_;
  std::vector<std::pair<std::string, std::string>> mount_points_;

  void server_loop();
  std::string create_error_response(const std::string& message,
                                    int status_code);
  std::string create_success_response(const nlohmann::json& data);
};

}  // namespace quantclaw::web
