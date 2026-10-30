import { act, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { MinuteRetirementList, MinuteRetirementPanel } from "./MinuteRetirementPanel";
import type { MinuteRetirement, Snapshot } from "../../src/bridge/client";
import { readySnapshot } from "../../src/testing/core";
import { BackendError, type TerminalContext } from "../contract";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { feature } from "./feature";
registerTerminalFeatures([feature]);
const id = "a".repeat(64);
const report = (state: MinuteRetirement["state"]): MinuteRetirement => ({
  id,
  state,
  payload_bytes: "4096",
  references: [],
  consumers: [],
});
const reply = (value: MinuteRetirement): Snapshot => ({
  ...readySnapshot(),
  minute_retirement: value,
});
const context = (
  query: TerminalContext["query"],
  trade: TerminalContext["trade"] = vi.fn(async () => {}),
): TerminalContext => ({
  snapshot: readySnapshot(),
  busy: false,
  error: "",
  navigate: () => {},
  openSettings: () => {},
  query,
  trade,
});

test("prepared and confirmed references block deletion without an override", async () => {
  const value = report("blocked");
  value.consumers = ["prepared", "confirmed"].map((phase, index) => ({
    node: "stable-node",
    service: "research",
    domain: "research",
    kind: "backtest",
    id: `run-${index}`,
    version: "b".repeat(64),
    phase: phase as "prepared" | "confirmed",
  }));
  const ctx = context(async () => reply(value));
  render(<MinuteRetirementPanel context={ctx} id={id} onClose={() => {}} onRemoved={() => {}} />);
  await screen.findByText("准备中的引用仍保留", { exact: false });
  expect(screen.getByText("正式引用仍保留", { exact: false })).toBeTruthy();
  expect(screen.queryByRole("checkbox")).toBeNull();
  expect(screen.queryByRole("button", { name: "删除此版本" })).toBeNull();
  expect(ctx.trade).not.toHaveBeenCalled();
});

test("explicit confirmation executes once and a new reference still blocks completion", async () => {
  let state: MinuteRetirement["state"] = "available";
  const trade = vi.fn(async () => {
    state = "blocked";
  });
  const ctx = context(async () => reply(report(state)), trade);
  const removed = vi.fn();
  render(<MinuteRetirementPanel context={ctx} id={id} onClose={() => {}} onRemoved={removed} />);
  const button = await screen.findByRole("button", { name: "删除此版本" });
  expect((button as HTMLButtonElement).disabled).toBe(true);
  fireEvent.click(screen.getByRole("checkbox"));
  fireEvent.click(button);
  await screen.findByText("有关联记录，禁止删除");
  expect(trade).toHaveBeenCalledExactlyOnceWith("data.history.retirement.execute", { id });
  expect(removed).not.toHaveBeenCalled();
});

test("uncertain execution clears permission until original service state is inspected", async () => {
  let state: MinuteRetirement["state"] = "pending";
  const trade = vi.fn(async () => {
    state = "removed";
    throw new BackendError("unavailable", "lost reply");
  });
  const ctx = context(async () => reply(report(state)), trade);
  render(<MinuteRetirementPanel context={ctx} id={id} onClose={() => {}} onRemoved={() => {}} />);
  await screen.findByRole("button", { name: "继续清理" });
  fireEvent.click(screen.getByRole("checkbox"));
  fireEvent.click(screen.getByRole("button", { name: "继续清理" }));
  await screen.findByRole("alert");
  expect(screen.queryByRole("checkbox")).toBeNull();
  fireEvent.click(screen.getByRole("button", { name: "刷新维护预览" }));
  await screen.findByText("载荷已删除，来源证据保留");
  expect(trade).toHaveBeenCalledTimes(1);
});

test("a stale preview cannot enable deletion of another version", async () => {
  let release!: (reply: Snapshot) => void;
  const held = new Promise<Snapshot>(resolve => {
    release = resolve;
  });
  const other = "c".repeat(64);
  const ctx = context(async (...args) => {
    if (args[0] !== "data.history.retirement.preview")
      throw new Error("unexpected fixture request");
    return args[1].id === id ? held : reply({ ...report("blocked"), id: other });
  });
  const props = { context: ctx, onClose: () => {}, onRemoved: () => {} };
  const view = render(<MinuteRetirementPanel {...props} id={id} />);
  view.rerender(<MinuteRetirementPanel {...props} id={other} />);
  await screen.findByText("有关联记录，禁止删除");
  await act(async () => release(reply(report("available"))));
  expect(screen.queryByRole("button", { name: "删除此版本" })).toBeNull();
});

test("retirement inventory is explicit and can reopen an interrupted cleanup", async () => {
  const query = vi.fn(async () => ({
    ...readySnapshot(),
    minute_retirements: { items: [report("pending")], next_after: "" },
  }));
  const ctx = context(query);
  ctx.snapshot = { ...readySnapshot(), data: { ...readySnapshot().data!, online: true } };
  const open = vi.fn();
  render(<MinuteRetirementList context={ctx} onOpen={open} />);
  expect(query).not.toHaveBeenCalled();
  fireEvent.click(screen.getByRole("button", { name: "查看封存与清理记录" }));
  await waitFor(() =>
    expect(query).toHaveBeenCalledExactlyOnceWith("data.history.retirements", { after: "" }),
  );
  fireEvent.click(await screen.findByRole("button", { name: "核对维护状态" }));
  expect(open).toHaveBeenCalledExactlyOnceWith(id);
});
