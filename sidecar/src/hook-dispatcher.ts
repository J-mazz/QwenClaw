// Copyright 2024 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0

// ---------------------------------------------------------------------------
// Hook dispatcher — fires hooks using the correct OpenClaw execution mode.
//
// Three modes:
//   void      — fire-and-forget, all handlers run in parallel
//   modifying — sequential, results merged via Object.assign
//   sync      — synchronous only; Promise returns are rejected
// ---------------------------------------------------------------------------

import type { HookHandler, HookMode, HookRegistration, PluginLogger } from "./types.js";
import { HOOK_MODES } from "./types.js";

export class HookDispatcher {
  private hooks: HookRegistration[] = [];
  private readonly logger: PluginLogger;

  constructor(logger: PluginLogger) {
    this.logger = logger;
  }

  /** Register a hook handler. */
  register(reg: HookRegistration): void {
    this.hooks.push(reg);
    // Keep sorted by priority descending (higher runs first).
    this.hooks.sort((a, b) => b.priority - a.priority);
  }

  /** Remove all registered hooks (used for plugin reload). */
  clear(): void {
    this.hooks = [];
  }

  /** Get the execution mode for a hook. Defaults to "void" for unknown hooks. */
  getMode(hookName: string): HookMode {
    return HOOK_MODES[hookName] ?? "void";
  }

  /** Return all registered hook names (deduplicated). */
  registeredHooks(): string[] {
    return [...new Set(this.hooks.map((h) => h.hookName))];
  }

  /** Count handlers for a specific hook. */
  handlerCount(hookName: string): number {
    return this.hooks.filter((h) => h.hookName === hookName).length;
  }

  /**
   * Fire a hook event and return the merged result.
   * Dispatches to the correct execution mode automatically.
   */
  async fire(
    hookName: string,
    event: Record<string, unknown>,
  ): Promise<Record<string, unknown> | undefined> {
    const handlers = this.hooks.filter((h) => h.hookName === hookName);
    if (handlers.length === 0) return undefined;

    const mode = this.getMode(hookName);

    switch (mode) {
      case "void":
        return this.fireVoid(hookName, handlers, event);
      case "modifying":
        return this.fireModifying(hookName, handlers, event);
      case "sync":
        return this.fireSync(hookName, handlers, event);
    }
  }

  // -----------------------------------------------------------------------
  // Void — parallel, no return value
  // -----------------------------------------------------------------------
  private async fireVoid(
    hookName: string,
    handlers: HookRegistration[],
    event: Record<string, unknown>,
  ): Promise<undefined> {
    this.logger.debug?.(`[hooks] firing ${hookName} (${handlers.length} handlers, void)`);

    const promises = handlers.map(async (h) => {
      try {
        await h.handler(event);
      } catch (err) {
        this.logger.error(
          `[hooks] ${hookName} handler from ${h.pluginId} failed: ${String(err)}`,
        );
      }
    });

    await Promise.all(promises);
    return undefined;
  }

  // -----------------------------------------------------------------------
  // Modifying — sequential, results merged
  // -----------------------------------------------------------------------
  private async fireModifying(
    hookName: string,
    handlers: HookRegistration[],
    event: Record<string, unknown>,
  ): Promise<Record<string, unknown> | undefined> {
    this.logger.debug?.(
      `[hooks] firing ${hookName} (${handlers.length} handlers, modifying)`,
    );

    let result: Record<string, unknown> | undefined;

    for (const h of handlers) {
      try {
        const out = await h.handler(event);
        if (out !== undefined && out !== null) {
          if (result !== undefined) {
            result = { ...result, ...out };
          } else {
            result = out;
          }
        }
      } catch (err) {
        this.logger.error(
          `[hooks] ${hookName} handler from ${h.pluginId} failed: ${String(err)}`,
        );
      }
    }

    return result;
  }

  // -----------------------------------------------------------------------
  // Sync — synchronous only, reject Promises
  // -----------------------------------------------------------------------
  private async fireSync(
    hookName: string,
    handlers: HookRegistration[],
    event: Record<string, unknown>,
  ): Promise<Record<string, unknown> | undefined> {
    this.logger.debug?.(
      `[hooks] firing ${hookName} (${handlers.length} handlers, sync)`,
    );

    let result: Record<string, unknown> | undefined;

    for (const h of handlers) {
      try {
        const out = h.handler(event) as unknown;

        // Guard against accidental async handlers.
        if (out && typeof (out as { then?: unknown }).then === "function") {
          this.logger.warn(
            `[hooks] ${hookName} handler from ${h.pluginId} returned a Promise; ` +
            `this hook is synchronous and the result was ignored.`,
          );
          continue;
        }

        const typed = out as Record<string, unknown> | void;
        if (typed !== undefined && typed !== null) {
          if (result !== undefined) {
            result = { ...result, ...typed };
          } else {
            result = typed as Record<string, unknown>;
          }
        }
      } catch (err) {
        this.logger.error(
          `[hooks] ${hookName} handler from ${h.pluginId} failed: ${String(err)}`,
        );
      }
    }

    return result;
  }
}
