// Copyright 2024 QuantClaw Contributors
// SPDX-License-Identifier: Apache-2.0

// ---------------------------------------------------------------------------
// Tool executor — manages tools registered by plugins and executes them.
// ---------------------------------------------------------------------------

import type { AgentTool, ToolEntry, ToolSchema } from "./types.js";

export class ToolExecutor {
  private tools = new Map<string, ToolEntry>();

  /** Register a tool from a plugin. */
  register(pluginId: string, tool: AgentTool): void {
    this.tools.set(tool.name, { pluginId, tool });
  }

  /** Remove all registered tools (used for plugin reload). */
  clear(): void {
    this.tools.clear();
  }

  /** Check if a tool exists. */
  has(name: string): boolean {
    return this.tools.has(name);
  }

  /** List all tool names. */
  toolNames(): string[] {
    return [...this.tools.keys()];
  }

  /** Get tools owned by a specific plugin. */
  toolsForPlugin(pluginId: string): string[] {
    const result: string[] = [];
    for (const [name, entry] of this.tools) {
      if (entry.pluginId === pluginId) result.push(name);
    }
    return result;
  }

  /** Return JSON schemas for all registered tools. */
  getSchemas(): ToolSchema[] {
    const schemas: ToolSchema[] = [];
    for (const entry of this.tools.values()) {
      const t = entry.tool;
      schemas.push({
        name: t.name,
        description: t.description,
        inputSchema: t.inputSchema ?? t.parameters,
      });
    }
    return schemas;
  }

  /** Execute a tool by name. */
  async execute(toolName: string, args: Record<string, unknown>): Promise<unknown> {
    const entry = this.tools.get(toolName);
    if (!entry) {
      throw new Error(`Tool not found: ${toolName}`);
    }
    return entry.tool.run(args);
  }
}
