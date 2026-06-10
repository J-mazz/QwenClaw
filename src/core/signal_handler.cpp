// Copyright 2025 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0

module;

#include <csignal>
#include <unistd.h>

module quantclaw.core.signal_handler;

import std;

namespace quantclaw {

std::atomic<bool> SignalHandler::shutdown_requested_{false};
std::atomic<bool> SignalHandler::reload_requested_{false};
SignalHandler::ShutdownCallback SignalHandler::shutdown_callback_;
SignalHandler::ReloadCallback SignalHandler::reload_callback_;

void SignalHandler::Install(ShutdownCallback on_shutdown,
                            ReloadCallback on_reload) {
  shutdown_callback_ = std::move(on_shutdown);
  reload_callback_ = std::move(on_reload);
  shutdown_requested_ = false;
  reload_requested_ = false;

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);
#ifndef _WIN32
  std::signal(SIGUSR1, signal_handler);
  std::signal(SIGHUP, SIG_IGN);
#endif
}

void SignalHandler::WaitForShutdown() {
  // Callbacks are dispatched here, on the waiting thread, rather than from
  // the signal handler itself: arbitrary std::function bodies (logging,
  // allocation, locking) are not async-signal-safe.
  while (!shutdown_requested_) {
    if (reload_requested_.exchange(false) && reload_callback_) {
      reload_callback_();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (shutdown_callback_) {
    shutdown_callback_();
  }
}

bool SignalHandler::ShouldShutdown() {
  return shutdown_requested_;
}

void SignalHandler::signal_handler(int signum) {
  if (signum == SIGINT || signum == SIGTERM) {
    // Prevent re-entrant shutdown: if already requested, force-exit.
    // Use _exit() which is async-signal-safe, unlike std::signal/std::raise.
    if (shutdown_requested_.exchange(true)) {
      _exit(128 + signum);
    }
  }
#ifndef _WIN32
  else if (signum == SIGUSR1) {
    reload_requested_.store(true);
  }
#endif
}

}  // namespace quantclaw
