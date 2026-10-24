import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { test, expect, vi } from "vitest";
import { readySnapshot } from "../../src/testing/core";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { taskOwner } from "../../src/bridge/client";
import { EventSetReview } from "./EventSetReview";
import { plugin } from "./plugin";
registerTerminalPlugins([plugin]);
test("fixed set audit submits its source and purpose with no Data selection or cutoff override", async () => {
  const snapshot = readySnapshot();
  snapshot.data = null;
  snapshot.research_service!.owner_node_id = "stable";
  const source = {
    node: "stable",
    service: "intel",
    domain: "intelligence",
    kind: "intelligence_set",
    id: "fixed-set",
    version: "a".repeat(64),
  };
  const trade = vi.fn(async () => {});
  render(
    <EventSetReview
      snapshot={snapshot}
      busy={false}
      error=""
      query={vi.fn(async () => snapshot)}
      trade={trade}
      navigate={vi.fn()}
      openSettings={vi.fn()}
      workspaceParams={{ source: JSON.stringify(source), scope: "research" }}
    />,
  );
  fireEvent.change(screen.getByLabelText("集合审阅任务 ID"), { target: { value: "set-audit" } });
  fireEvent.click(screen.getByRole("button", { name: "提交或确认原集合审阅" }));
  await waitFor(() =>
    expect(trade).toHaveBeenCalledWith("event.set.review.submit", {
      id: "set-audit",
      owner: taskOwner(snapshot.research_service, "research"),
      source,
      use_scope: "research",
    }),
  );
  expect(screen.queryByLabelText("集合截止时间（UTC）")).toBeNull();
});
