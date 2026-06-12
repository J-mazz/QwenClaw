// Copyright 2024 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0

import { describe, it, expect, beforeEach } from "vitest";
import { HookDispatcher } from "../src/hook-dispatcher.js";
import type { PluginLogger } from "../src/types.js";

function createTestLogger(): PluginLogger & { messages: string[] } {
  const messages: string[] = [];
  return {
    messages,
    debug: (msg: string) => messages.push(`debug: ${msg}`),
    info: (msg: string) => messages.push(`info: ${msg}`),
    warn: (msg: string) => messages.push(`warn: ${msg}`),
    error: (msg: string) => messages.push(`error: ${msg}`),
  };
}

describe("HookDispatcher", () => {
  let dispatcher: HookDispatcher;
  let logger: ReturnType<typeof createTestLogger>;

  beforeEach(() => {
    logger = createTestLogger();
    dispatcher = new HookDispatcher(logger);
  });

  // -----------------------------------------------------------------------
  // Registration
  // -----------------------------------------------------------------------

  describe("registration", () => {
    it("should register hooks and report them", () => {
      dispatcher.register({
        pluginId: "test",
        hookName: "message_received",
        handler: async () => {},
        priority: 0,
      });

      expect(dispatcher.registeredHooks()).toEqual(["message_received"]);
      expect(dispatcher.handlerCount("message_received")).toBe(1);
    });

    it("should sort handlers by priority descending", async () => {
      const order: number[] = [];

      dispatcher.register({
        pluginId: "low",
        hookName: "before_model_resolve",
        handler: async () => { order.push(1); return {}; },
        priority: 1,
      });
      dispatcher.register({
        pluginId: "high",
        hookName: "before_model_resolve",
        handler: async () => { order.push(100); return {}; },
        priority: 100,
      });
      dispatcher.register({
        pluginId: "mid",
        hookName: "before_model_resolve",
        handler: async () => { order.push(50); return {}; },
        priority: 50,
      });

      await dispatcher.fire("before_model_resolve", {});
      expect(order).toEqual([100, 50, 1]);
    });

    it("should remove all hooks on clear", () => {
      dispatcher.register({
        pluginId: "test",
        hookName: "message_received",
        handler: async () => {},
        priority: 0,
      });

      dispatcher.clear();

      expect(dispatcher.registeredHooks()).toEqual([]);
      expect(dispatcher.handlerCount("message_received")).toBe(0);
    });
  });

  // -----------------------------------------------------------------------
  // Void hooks — fire-and-forget, parallel
  // -----------------------------------------------------------------------

  describe("void hooks", () => {
    it("should classify message_received as void", () => {
      expect(dispatcher.getMode("message_received")).toBe("void");
    });

    it("should fire all handlers in parallel", async () => {
      let count = 0;
      for (let i = 0; i < 3; i++) {
        dispatcher.register({
          pluginId: `p${i}`,
          hookName: "message_received",
          handler: async () => { count++; },
          priority: 0,
        });
      }

      const result = await dispatcher.fire("message_received", { text: "hello" });
      expect(count).toBe(3);
      expect(result).toBeUndefined();
    });

    it("should not crash when a void handler throws", async () => {
      dispatcher.register({
        pluginId: "bad",
        hookName: "agent_end",
        handler: async () => { throw new Error("boom"); },
        priority: 0,
      });
      dispatcher.register({
        pluginId: "good",
        hookName: "agent_end",
        handler: async () => {},
        priority: 0,
      });

      // Should not throw.
      await dispatcher.fire("agent_end", {});
      expect(logger.messages.some((m) => m.includes("boom"))).toBe(true);
    });

    it("should return undefined for void hooks", async () => {
      dispatcher.register({
        pluginId: "test",
        hookName: "llm_input",
        handler: async () => ({ modified: true }),
        priority: 0,
      });

      const result = await dispatcher.fire("llm_input", {});
      // Void hooks discard return values.
      expect(result).toBeUndefined();
    });
  });

  // -----------------------------------------------------------------------
  // Modifying hooks — sequential, results merged
  // -----------------------------------------------------------------------

  describe("modifying hooks", () => {
    it("should classify before_model_resolve as modifying", () => {
      expect(dispatcher.getMode("before_model_resolve")).toBe("modifying");
    });

    it("should run handlers sequentially and merge results", async () => {
      dispatcher.register({
        pluginId: "a",
        hookName: "before_prompt_build",
        handler: async () => ({ context: "extra context" }),
        priority: 10,
      });
      dispatcher.register({
        pluginId: "b",
        hookName: "before_prompt_build",
        handler: async () => ({ systemPrompt: "be helpful" }),
        priority: 5,
      });

      const result = await dispatcher.fire("before_prompt_build", {});
      expect(result).toEqual({
        context: "extra context",
        systemPrompt: "be helpful",
      });
    });

    it("should let later handlers override earlier ones", async () => {
      dispatcher.register({
        pluginId: "first",
        hookName: "before_model_resolve",
        handler: async () => ({ model: "gpt-4" }),
        priority: 10,
      });
      dispatcher.register({
        pluginId: "second",
        hookName: "before_model_resolve",
        handler: async () => ({ model: "claude-3" }),
        priority: 5,
      });

      const result = await dispatcher.fire("before_model_resolve", {});
      expect(result).toEqual({ model: "claude-3" });
    });

    it("should skip null/undefined results from handlers", async () => {
      dispatcher.register({
        pluginId: "noop",
        hookName: "message_sending",
        handler: async () => undefined,
        priority: 10,
      });
      dispatcher.register({
        pluginId: "real",
        hookName: "message_sending",
        handler: async () => ({ content: "modified" }),
        priority: 5,
      });

      const result = await dispatcher.fire("message_sending", {});
      expect(result).toEqual({ content: "modified" });
    });

    it("should continue after handler error in modifying mode", async () => {
      dispatcher.register({
        pluginId: "bad",
        hookName: "before_tool_call",
        handler: async () => { throw new Error("fail"); },
        priority: 10,
      });
      dispatcher.register({
        pluginId: "good",
        hookName: "before_tool_call",
        handler: async () => ({ allowed: true }),
        priority: 5,
      });

      const result = await dispatcher.fire("before_tool_call", {});
      expect(result).toEqual({ allowed: true });
    });
  });

  // -----------------------------------------------------------------------
  // Sync hooks — synchronous only
  // -----------------------------------------------------------------------

  describe("sync hooks", () => {
    it("should classify tool_result_persist as sync", () => {
      expect(dispatcher.getMode("tool_result_persist")).toBe("sync");
    });

    it("should classify before_message_write as sync", () => {
      expect(dispatcher.getMode("before_message_write")).toBe("sync");
    });

    it("should execute synchronous handlers", async () => {
      dispatcher.register({
        pluginId: "sync-plugin",
        hookName: "tool_result_persist",
        handler: (event) => ({ message: { ...event, modified: true } }),
        priority: 0,
      });

      const result = await dispatcher.fire("tool_result_persist", {
        message: { role: "tool", content: "result" },
      });
      expect(result).toBeDefined();
    });

    it("should warn and skip when sync handler returns Promise", async () => {
      dispatcher.register({
        pluginId: "async-plugin",
        hookName: "tool_result_persist",
        handler: async () => ({ message: "should be ignored" }),
        priority: 0,
      });

      await dispatcher.fire("tool_result_persist", { message: {} });
      expect(
        logger.messages.some((m) => m.includes("returned a Promise")),
      ).toBe(true);
    });
  });

  // -----------------------------------------------------------------------
  // Edge cases
  // -----------------------------------------------------------------------

  describe("edge cases", () => {
    it("should return undefined for no handlers", async () => {
      const result = await dispatcher.fire("nonexistent_hook", {});
      expect(result).toBeUndefined();
    });

    it("should default unknown hooks to void mode", () => {
      expect(dispatcher.getMode("some_future_hook")).toBe("void");
    });

    it("should handle all 24 hook names", () => {
      const hookNames = [
        "before_model_resolve", "before_prompt_build", "before_agent_start",
        "llm_input", "llm_output", "agent_end",
        "before_compaction", "after_compaction", "before_reset",
        "message_received", "message_sending", "message_sent",
        "before_tool_call", "after_tool_call",
        "tool_result_persist", "before_message_write",
        "session_start", "session_end",
        "subagent_spawning", "subagent_delivery_target",
        "subagent_spawned", "subagent_ended",
        "gateway_start", "gateway_stop",
      ];

      for (const name of hookNames) {
        const mode = dispatcher.getMode(name);
        expect(["void", "modifying", "sync"]).toContain(mode);
      }
    });
  });
});
