import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test } from "vitest";
import type { PythonRuntime } from "../bridge/client";
import { readySnapshot, serveCore } from "../testing/core";
import { SetupGate } from "./SetupGate";

const start = () =>
  render(
    <SetupGate>
      <p>workbench</p>
    </SetupGate>,
  );
const workbench = () => screen.queryByText("workbench");
const modules = (state: PythonRuntime["modules"][number]["state"], done = 0) =>
  [
    { id: "interpreter", title: "Python 解释器", size: 100, done, state },
    { id: "core", title: "核心", size: 300, done: done * 3, state },
  ] satisfies PythonRuntime["modules"];

test("an installed runtime opens the workbench at once and restores services behind it", async () => {
  const calls = serveCore(({ method }) => ({
    ...readySnapshot(),
    python_runtime: { state: "ready", error: "", modules: modules("done", 100) },
    ...(method === "runtime.python.install" ? { unexpected: true } : {}),
  }));
  start();
  await waitFor(() => expect(workbench()).not.toBeNull());
  await waitFor(() => expect(calls.map(call => call.method)).toContain("market.local"));
  expect(calls.map(call => call.method)).not.toContain("runtime.python.install");
});

test("the first launch installs the runtime, one row per module, then enters", async () => {
  let polls = 0;
  const calls = serveCore(({ method }) => {
    const snapshot = readySnapshot();
    if (method === "runtime.python.install")
      return {
        ...snapshot,
        python_runtime: { state: "installing", error: "", modules: modules("downloading") },
      };
    if (method === "runtime.python.status") {
      polls++;
      const state = polls === 1 ? "missing" : polls === 2 ? "installing" : "ready";
      return {
        ...snapshot,
        python_runtime: {
          state,
          error: "",
          modules:
            state === "ready"
              ? modules("done", 100)
              : modules(state === "missing" ? "waiting" : "downloading", 46),
        },
      };
    }
    return snapshot;
  });
  start();
  await screen.findByRole("progressbar", { name: "核心" });
  expect(screen.getByText(/第一次启动/).textContent).toContain("0 MB");
  await waitFor(() => expect(workbench()).not.toBeNull(), { timeout: 3000 });
  expect(calls.map(call => call.method)).toContain("runtime.python.install");
});

test("a failed install says why and retries", async () => {
  let failed = true;
  serveCore(({ method }) => {
    const snapshot = readySnapshot();
    if (method === "runtime.python.install") failed = false;
    return {
      ...snapshot,
      python_runtime: failed
        ? { state: "failed", error: "cannot download core", modules: modules("failed", 40) }
        : { state: "ready", error: "", modules: modules("done", 100) },
    };
  });
  start();
  expect((await screen.findByRole("alert")).textContent).toContain("cannot download core");
  expect(workbench()).toBeNull();
  fireEvent.click(screen.getByRole("button", { name: "重试" }));
  await waitFor(() => expect(workbench()).not.toBeNull());
});
