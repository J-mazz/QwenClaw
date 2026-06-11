import { describe, expect, it } from "vitest";
import { beginLoad } from "./load-guard.ts";

describe("beginLoad", () => {
  it("invalidates earlier loads with the same key", () => {
    const state = {};
    const first = beginLoad(state, "jobs");
    expect(first()).toBe(true);
    const second = beginLoad(state, "jobs");
    expect(first()).toBe(false);
    expect(second()).toBe(true);
  });

  it("tracks keys independently", () => {
    const state = {};
    const jobs = beginLoad(state, "jobs");
    const runs = beginLoad(state, "runs");
    beginLoad(state, "jobs");
    expect(jobs()).toBe(false);
    expect(runs()).toBe(true);
  });

  it("scopes sequences per state object", () => {
    const a = {};
    const b = {};
    const loadA = beginLoad(a, "jobs");
    beginLoad(b, "jobs");
    expect(loadA()).toBe(true);
  });
});
