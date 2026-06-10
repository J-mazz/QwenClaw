import { afterEach, beforeEach } from "vitest";
// Side-effect import: registers the <quantclaw-app> custom element. A bare
// `import { QuantClawApp }` gets elided by the TS transform because the
// binding is only used in type positions, leaving the element undefined.
import "../app.ts";
import type { QuantClawApp } from "../app.ts";

export function mountApp(pathname: string) {
  window.history.replaceState({}, "", pathname);
  const app = document.createElement("quantclaw-app") as QuantClawApp;
  app.connect = () => {
    // no-op: avoid real gateway WS connections in browser tests
  };
  document.body.append(app);
  return app;
}

export function registerAppMountHooks() {
  beforeEach(() => {
    window.__QUANTCLAW_CONTROL_UI_BASE_PATH__ = undefined;
    localStorage.clear();
    document.body.innerHTML = "";
  });

  afterEach(() => {
    window.__QUANTCLAW_CONTROL_UI_BASE_PATH__ = undefined;
    localStorage.clear();
    document.body.innerHTML = "";
  });
}
