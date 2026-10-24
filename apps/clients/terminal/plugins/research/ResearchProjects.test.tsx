import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import { readySnapshot } from "../../src/testing/core";
import type { ResearchHypothesisPlan, ResearchPlanBinding } from "../../src/bridge/client";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import type { TerminalContext } from "../contract";
import { ResearchProjects } from "./ResearchProjects";
import { ResearchPlanPicker } from "./ResearchPlan";
import { plugin } from "./plugin";
registerTerminalPlugins([plugin]);

test("unsaved project drafts survive navigation and warn until the exact saved draft is confirmed", async () => {
  const snapshot = readySnapshot();
  const store = createWorkspaceDrafts();
  let saved: Parameters<TerminalContext["trade"]>[1] | null = null;
  const trade = vi.fn(async (...[method, params]: Parameters<TerminalContext["trade"]>) => {
    if (method === "research.project.save") saved = params;
  });
  const query: TerminalContext["query"] = async (...[method]) => {
    const value = saved as { project_id: string; definition: Record<string, unknown> } | null;
    return {
      ...snapshot,
      research_project: {
        drafts: [],
        conclusions: [],
        plans: [],
        next_after: "",
        ...(method === "research.project.read" && value
          ? {
              draft: {
                id: value.project_id,
                owner: { domain: "research" as const, node_id: "local", service_id: "research" },
                definition: value.definition as unknown as ResearchHypothesisPlan["definition"],
                digest: "a".repeat(64),
                updated_at_ms: "1",
              },
            }
          : {}),
      },
    };
  };
  const context: TerminalContext = {
    snapshot,
    query,
    trade,
    busy: false,
    error: "",
    navigate: vi.fn(),
    openSettings: vi.fn(),
  };
  const view = () => (
    <WorkspaceDraftScope store={store} namespace="projects">
      <ResearchProjects {...context} />
    </WorkspaceDraftScope>
  );
  const mounted = render(view());
  fireEvent.change(screen.getByLabelText("项目标题"), { target: { value: "Original criteria" } });
  const pending = new Event("beforeunload", { cancelable: true });
  window.dispatchEvent(pending);
  expect(pending.defaultPrevented).toBe(true);
  mounted.unmount();
  const away = new Event("beforeunload", { cancelable: true });
  window.dispatchEvent(away);
  expect(away.defaultPrevented).toBe(true);
  const reopened = render(view());
  expect((screen.getByLabelText("项目标题") as HTMLTextAreaElement).value).toBe(
    "Original criteria",
  );
  fireEvent.click(screen.getByRole("button", { name: "保存研究草稿" }));
  await waitFor(() => expect(screen.getByText("草稿已保存", { exact: true })).toBeTruthy());
  expect(trade).toHaveBeenCalledTimes(1);
  const clean = new Event("beforeunload", { cancelable: true });
  window.dispatchEvent(clean);
  expect(clean.defaultPrevented).toBe(false);
  expect((screen.getByRole("button", { name: "固定计划" }) as HTMLButtonElement).disabled).toBe(
    true,
  );
  reopened.unmount();
  store.dispose();
});

test("a persisted plan from another Research owner stays blocked until explicitly cleared", async () => {
  const snapshot = readySnapshot();
  const binding: ResearchPlanBinding = {
    project_id: "project-a",
    plan_id: "plan-a",
    definition_digest: "a".repeat(64),
    owner: { domain: "research", node_id: "another-node", service_id: "research" },
  };
  const ready = vi.fn();
  const change = vi.fn();
  const query = vi.fn(async () => snapshot) as TerminalContext["query"];
  render(
    <ResearchPlanPicker
      snapshot={snapshot}
      query={query}
      binding={binding}
      onReady={ready}
      onChange={change}
    />,
  );
  await waitFor(() => expect(ready).toHaveBeenCalledWith(false));
  expect(ready).not.toHaveBeenCalledWith(true);
  expect(change).not.toHaveBeenCalled();
  fireEvent.change(screen.getByRole("combobox", { name: "选择固定计划" }), {
    target: { value: "" },
  });
  expect(change).toHaveBeenCalledWith(null);
});
