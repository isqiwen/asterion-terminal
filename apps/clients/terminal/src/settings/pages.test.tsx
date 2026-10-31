import { fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { localService, readySnapshot } from "../testing/core";
import { Connections } from "./Connections";

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

test("every settings page has its title, and trading connections have their own page", () => {
  const props = {
    snapshot: readySnapshot(),
    refresh: () => {},
    error: "",
    busy: false,
    trade: vi.fn().mockResolvedValue(undefined),
  };
  const { unmount } = render(<Settings {...props} initialPage="research" />);
  expect(screen.getByRole("heading", { level: 1 }).textContent).toBe("研究");
  unmount();
  render(<Settings {...props} initialPage="trading" />);
  expect(screen.getByRole("heading", { level: 1 }).textContent).toBe("交易连接");
  expect(screen.getByRole("button", { name: "添加交易连接" })).toBeTruthy();
  expect(screen.queryByRole("region", { name: "CTP 行情账户" })).toBeNull();
});

test("data sources hold CTP quotes, the recording host and history vendors together", async () => {
  const snapshot = readySnapshot();
  snapshot.recorder_endpoint = null;
  const trade = vi.fn().mockResolvedValue(undefined);
  render(
    <Settings
      snapshot={snapshot}
      refresh={() => {}}
      error=""
      busy={false}
      trade={trade}
      initialPage="sources"
    />,
  );
  for (const name of ["实时行情（CTP）", "行情录制主机", "历史数据供应商"])
    expect(screen.getByRole("heading", { name, level: 2 })).toBeTruthy();
  expect(screen.queryByRole("button", { name: "CTP 行情账户" })).toBeNull();
  const recorder = screen.getByRole("region", { name: "行情录制主机" });
  fireEvent.click(within(recorder).getByRole("button", { name: "填写地址" }));
  fireEvent.change(within(recorder).getByLabelText("主机地址"), {
    target: { value: " 192.168.1.20 " },
  });
  fireEvent.click(within(recorder).getByRole("button", { name: "保存地址" }));
  await waitFor(() =>
    expect(trade).toHaveBeenCalledWith("recorder.endpoint.save", {
      host: "192.168.1.20",
      port: 7450,
    }),
  );
});
