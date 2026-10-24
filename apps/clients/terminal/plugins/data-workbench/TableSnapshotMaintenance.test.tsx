import { fireEvent, render, screen } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { readySnapshot } from "../../src/testing/core";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import type { TableRetirement } from "../../src/bridge/client";
import type { TerminalContext } from "../contract";
import { plugin } from "./plugin";
import { TableSnapshotMaintenance } from "./TableSnapshotMaintenance";
registerTerminalPlugins([plugin]);
const id = "a".repeat(64);
const available: TableRetirement = {
  preview_digest: "b".repeat(64),
  id,
  sequence: "1",
  current_head: false,
  payload_bytes: "0",
  state: "available",
  references: [],
  consumers: [],
  acquisitions: [],
  objects: [],
};
function context(report: TableRetirement) {
  return {
    snapshot: readySnapshot(),
    busy: false,
    error: "",
    query: vi.fn(async () => ({ ...readySnapshot(), table_retirement: report })),
    trade: vi.fn(async () => undefined),
    navigate: vi.fn(),
    openSettings: vi.fn(),
  } satisfies TerminalContext;
}
test("zero reclaimable bytes still requires an exact preview and explicit confirmation", async () => {
  const ctx = context(available);
  ctx.query
    .mockResolvedValueOnce({ ...readySnapshot(), table_retirement: available })
    .mockResolvedValue({
      ...readySnapshot(),
      table_retirement: { ...available, state: "removed" },
    });
  render(<TableSnapshotMaintenance context={ctx} />);
  expect(ctx.query).not.toHaveBeenCalled();
  fireEvent.click(screen.getByText("表快照维护", { selector: "summary" }));
  fireEvent.change(screen.getByLabelText("待维护快照 SHA-256"), { target: { value: id } });
  fireEvent.click(screen.getByRole("button", { name: "预览表快照维护" }));
  await screen.findByText("无阻断项，可退役快照");
  expect(screen.getByText("预计独占载荷字节", { exact: false }).textContent).toContain("0");
  expect((screen.getByRole("button", { name: "退役此表快照" }) as HTMLButtonElement).disabled).toBe(
    true,
  );
  fireEvent.click(screen.getByRole("checkbox"));
  fireEvent.click(screen.getByRole("button", { name: "退役此表快照" }));
  await screen.findByText("快照已退役，独占载荷已清理");
  expect(ctx.trade).toHaveBeenCalledExactlyOnceWith("data.tables.retirement.execute", {
    id,
    preview_digest: available.preview_digest,
  });
});
test("head, durable consumers and local drafts never expose deletion confirmation", async () => {
  const ctx = context({
    ...available,
    state: "blocked",
    current_head: true,
    acquisitions: ["acquisition"],
    references: [{ kind: "saved_dataset", id: "saved", name: "retained", roles: ["market"] }],
  });
  render(<TableSnapshotMaintenance context={ctx} />);
  fireEvent.click(screen.getByText("表快照维护", { selector: "summary" }));
  fireEvent.change(screen.getByLabelText("待维护快照 SHA-256"), { target: { value: id } });
  fireEvent.click(screen.getByRole("button", { name: "预览表快照维护" }));
  await screen.findByText("快照仍被使用，禁止退役");
  expect(screen.getByText("当前头快照不能退役")).toBeTruthy();
  expect(screen.getByText("retained")).toBeTruthy();
  expect(screen.getByText("acquisition")).toBeTruthy();
  expect(screen.queryByRole("checkbox")).toBeNull();
  expect(ctx.trade).not.toHaveBeenCalled();
});
