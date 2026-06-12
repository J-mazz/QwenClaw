// Copyright 2024 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0

import { describe, it, expect } from "vitest";
import { ToolExecutor } from "../src/tool-executor.js";

function makeTool(name: string) {
  return {
    name,
    description: `tool ${name}`,
    run: async () => ({ ok: true, name }),
  };
}

describe("ToolExecutor", () => {
  it("registers and executes tools", async () => {
    const tools = new ToolExecutor();
    tools.register("plugin-a", makeTool("alpha"));

    expect(tools.has("alpha")).toBe(true);
    expect(tools.toolNames()).toEqual(["alpha"]);
    expect(await tools.execute("alpha", {})).toEqual({ ok: true, name: "alpha" });
  });

  it("removes all tools on clear", () => {
    const tools = new ToolExecutor();
    tools.register("plugin-a", makeTool("alpha"));
    tools.register("plugin-b", makeTool("beta"));

    tools.clear();

    expect(tools.toolNames()).toEqual([]);
    expect(tools.has("alpha")).toBe(false);
    expect(tools.getSchemas()).toEqual([]);
  });

  it("throws for unknown tools", async () => {
    const tools = new ToolExecutor();
    await expect(tools.execute("missing", {})).rejects.toThrow("Tool not found");
  });
});
