import { fireEvent, render, screen } from "@testing-library/react";
import { expect, test } from "vitest";
import { readySnapshot, serveCore } from "../testing/core";
import type { ResourceCatalogue as Catalogue } from "../bridge/client";
import { ResourceCatalogue } from "./ResourceCatalogue";
const id = "a".repeat(64);
const page: Catalogue = {
  owner: { node: "stable-node", service: "historical-data", domain: "data" },
  kind: "minute_version",
  next_after: "",
  items: [
    {
      object: {
        node: "stable-node",
        service: "historical-data",
        domain: "data",
        kind: "minute_version",
        id,
        version: id,
      },
      state: "registered",
      name: "synthetic minute",
      logical_bytes: null,
      relative_location: "history/owned",
    },
  ],
};
test("owner catalogue shows unknown bytes and routes only to a read-only exact-object preview", async () => {
  const snapshot = readySnapshot();
  const calls = serveCore(call =>
    call.method === "resource.inspect"
      ? {
          ...snapshot,
          resource_inspection: {
            object: page.items[0].object,
            maintenance: {
              state: "blocked",
              consumers: [{ node: "stable-node", service: "research", id: "retained-run" }],
            },
          },
        }
      : { ...snapshot, resources: page },
  );
  render(<ResourceCatalogue snapshot={snapshot} />);
  expect(calls).toHaveLength(0);
  fireEvent.click(screen.getByRole("button", { name: "读取资产目录" }));
  await screen.findByText("synthetic minute");
  expect(screen.getByText("未知")).toBeTruthy();
  expect(screen.getByText(/逻辑载荷字节不等于磁盘独占占用/)).toBeTruthy();
  fireEvent.click(screen.getByRole("button", { name: "查看引用与维护" }));
  await screen.findByText("stable-node/research/retained-run");
  expect(calls).toEqual([
    { method: "resource.catalogue", params: { domain: "data", kind: "minute_version", after: "" } },
    { method: "resource.inspect", params: { object: page.items[0].object } },
  ]);
  expect(screen.queryByRole("checkbox")).toBeNull();
});
test("switching the owner discards the old catalogue and cannot inspect its entries", async () => {
  const snapshot = readySnapshot();
  serveCore(() => ({ ...snapshot, resources: page }));
  render(<ResourceCatalogue snapshot={snapshot} />);
  fireEvent.click(screen.getByRole("button", { name: "读取资产目录" }));
  await screen.findByText("synthetic minute");
  fireEvent.change(screen.getByLabelText("资产所属域"), { target: { value: "research" } });
  expect(screen.queryByText("synthetic minute")).toBeNull();
  expect(screen.queryByRole("button", { name: "查看引用与维护" })).toBeNull();
  expect((screen.getByLabelText("资产类别") as HTMLSelectElement).value).toBe("backtest");
});
