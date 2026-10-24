import { act, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { test, expect, vi } from "vitest";
import { readySnapshot } from "../../src/testing/core";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "../../src/host/workspace/drafts";
import type { Snapshot } from "../../src/bridge/client";
import type { TerminalContext } from "../contract";
import { NoticeSubscriptions } from "./NoticeSubscriptions";
import { plugin } from "./plugin";
registerTerminalPlugins([plugin]);

test("a late saved-subscription list cannot cross a changed owner connection or enable a business action", async () => {
  const snapshot = readySnapshot();
  snapshot.data = null;
  snapshot.intelligence_service = {
    node_id: "local",
    owner_node_id: "first-owner",
    service: "intel",
    connection_id: "first-connection",
    remote: false,
    host: "",
    port: 0,
    records: [],
  };
  let finish!: (value: Snapshot) => void;
  const query = vi.fn(
    () =>
      new Promise<Snapshot>(resolve => {
        finish = resolve;
      }),
  );
  const trade = vi.fn(async () => {});
  const context: TerminalContext = {
    snapshot,
    busy: false,
    error: "",
    query,
    trade,
    navigate: vi.fn(),
    openSettings: vi.fn(),
  };
  const drafts = createWorkspaceDrafts();
  const view = (ctx: TerminalContext) => (
    <WorkspaceDraftScope store={drafts} namespace="subscription-test">
      <NoticeSubscriptions context={ctx} />
    </WorkspaceDraftScope>
  );
  const rendered = render(view(context));
  fireEvent.click(screen.getByRole("button", { name: "读取已保存订阅" }));
  await waitFor(() => expect(query).toHaveBeenCalledWith("intelligence.subscription.list", {}));
  const next = {
    ...snapshot,
    intelligence_service: {
      ...snapshot.intelligence_service,
      owner_node_id: "second-owner",
      connection_id: "second-connection",
    },
  };
  rendered.rerender(view({ ...context, snapshot: next }));
  await act(async () =>
    finish({
      ...snapshot,
      notice_subscriptions: {
        owner: { node: "first-owner", service: "intel", domain: "intelligence" },
        head: "4",
        items: [],
      },
    }),
  );
  expect(screen.queryByRole("region", { name: "已保存订阅" })).toBeNull();
  expect(
    (screen.getByRole("button", { name: "确认启用应用内订阅" }) as HTMLButtonElement).disabled,
  ).toBe(true);
  expect(trade).not.toHaveBeenCalled();
});
