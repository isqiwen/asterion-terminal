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

test("Intelligence publication catalogue is read-only and clearing its selection discards old authority", async () => {
  const snapshot = readySnapshot();
  const node = snapshot.nodes[0];
  node.health!.services.push({
    ...node.health!.services[0],
    id: "intelligence-one",
    kind: "intelligence",
    health: "ready",
  });
  snapshot.intelligence_service = {
    node_id: node.id,
    owner_node_id: "stable-node",
    service: "intelligence-one",
    connection_id: "notice-connection",
    remote: false,
    host: "",
    port: 0,
    records: [],
  };
  const evidence: Catalogue = {
    ...page,
    owner: { node: "stable-node", service: "intelligence-one", domain: "intelligence" },
    kind: "intelligence_publication",
    items: [
      {
        ...page.items[0],
        object: {
          ...page.items[0].object,
          domain: "intelligence",
          service: "intelligence-one",
          kind: "intelligence_publication",
        },
        name: "fixed synthetic announcement",
      },
    ],
  };
  const calls = serveCore(() => ({ ...snapshot, resources: evidence }));
  const view = render(<ResourceCatalogue snapshot={snapshot} />);
  fireEvent.change(screen.getByLabelText("资产所属域"), { target: { value: "intelligence" } });
  fireEvent.change(screen.getByLabelText("资产类别"), {
    target: { value: "intelligence_publication" },
  });
  fireEvent.click(screen.getByRole("button", { name: "读取资产目录" }));
  await screen.findByText("fixed synthetic announcement");
  expect(screen.queryByRole("button", { name: "查看引用与维护" })).toBeNull();
  expect(screen.getByText(/此目录不提供更改或删除/)).toBeTruthy();
  expect(calls).toEqual([
    {
      method: "resource.catalogue",
      params: { domain: "intelligence", kind: "intelligence_publication", after: "" },
    },
  ]);
  view.rerender(<ResourceCatalogue snapshot={{ ...snapshot, intelligence_service: null }} />);
  expect(screen.queryByText("fixed synthetic announcement")).toBeNull();
});
