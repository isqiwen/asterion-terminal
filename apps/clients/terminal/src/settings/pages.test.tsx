import { fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { localService, readySnapshot } from "../testing/core";
import { Connections } from "./Connections";
import { DataSources } from "./DataSources";
import { NativePlugins } from "./NativePlugins";
import { Settings } from "./Settings";

// jsdom does not implement the browser's dialog methods.
HTMLDialogElement.prototype.showModal = function () {
  this.setAttribute("open", "");
};
HTMLDialogElement.prototype.close = function () {
  this.removeAttribute("open");
};

test("local services name what a state means and confirm before stopping", async () => {
  const snapshot = readySnapshot();
  localService(snapshot, "market-data").health = "awaiting_input";
  const trade = vi.fn().mockResolvedValue(undefined);
  render(<Connections snapshot={snapshot} busy={false} trade={trade} />);
  const market = screen.getByRole("listitem", { name: "market-data" });
  expect(market.textContent).toContain("未登录行情");
  expect(market.textContent).not.toContain("当前使用");
  fireEvent.click(within(market).getByRole("button", { name: "停止" }));
  expect(trade).not.toHaveBeenCalled();
  fireEvent.click(screen.getByRole("button", { name: "确认操作" }));
  await waitFor(() =>
    expect(trade).toHaveBeenCalledWith("node.action", { service: "market-data", action: "stop" }),
  );
});

test("a stopped Data service is started from the data source page, not Research", () => {
  const snapshot = readySnapshot();
  snapshot.data = null;
  const trade = vi.fn().mockResolvedValue(undefined);
  render(<DataSources snapshot={snapshot} busy={false} trade={trade} />);
  fireEvent.click(screen.getByRole("button", { name: "启动历史数据服务" }));
  expect(trade).toHaveBeenCalledWith("node.data.local.open");
  expect(screen.queryByText(/多节点/)).toBeNull();
});

test("plugins of a running service are changed after stopping it on the services page", () => {
  const snapshot = readySnapshot();
  const plugin = {
    file: "asterion_tushare.so",
    id: "asterion.data.tushare",
    version: "1.0.0",
    sha256: "a".repeat(64),
    capabilities: [{ id: "asterion.history", kind: "data", version: 3 }],
    selectable: true,
    state: "available" as const,
    error: "",
  };
  snapshot.native_plugins = { directory: "/app/plugins", items: [plugin] };
  localService(snapshot, "historical-data").plugin_artifacts = [plugin.sha256];
  const openServices = vi.fn();
  render(
    <NativePlugins
      snapshot={snapshot}
      busy={false}
      trade={vi.fn().mockResolvedValue(undefined)}
      openServices={openServices}
    />,
  );
  const data = screen.getByRole("region", { name: "服务插件 historical-data" });
  // A required plugin with one version is always on: text, not a locked checkbox.
  expect(within(data).queryByRole("checkbox")).toBeNull();
  expect(data.textContent).toContain("asterion.data.tushare · 1.0.0 · 随应用启用");
  expect(within(data).queryByRole("button", { name: "停止" })).toBeNull();
  fireEvent.click(within(data).getByRole("button", { name: "前往本机服务" }));
  expect(openServices).toHaveBeenCalled();
  // The catalog says what a plugin provides, not its capability identifiers.
  const catalog = screen.getByRole("table", { name: "原生插件目录" });
  expect(catalog.textContent).toContain("分钟 K 线");
  expect(catalog.textContent).not.toContain("asterion.history /");
});

test("every settings page has its title, and trading connections have their own page", () => {
  const props = {
    snapshot: readySnapshot(),
    refresh: () => {},
    error: "",
    busy: false,
    trade: vi.fn().mockResolvedValue(undefined),
  };
  const { unmount } = render(<Settings {...props} initialPage="resources" />);
  expect(screen.getByRole("heading", { level: 1 }).textContent).toBe("业务资产目录");
  unmount();
  render(<Settings {...props} initialPage="trading" />);
  expect(screen.getByRole("heading", { level: 1 }).textContent).toBe("交易连接");
  expect(screen.getByRole("button", { name: "添加交易连接" })).toBeTruthy();
  expect(screen.queryByRole("region", { name: "CTP 行情账户" })).toBeNull();
});
