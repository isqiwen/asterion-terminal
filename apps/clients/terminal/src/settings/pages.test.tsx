import { fireEvent, render, screen, waitFor, within, cleanup } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { readySnapshot } from "../testing/core";

import { Settings } from "./Settings";
import version from "../../../../../VERSION?raw";

// jsdom does not implement the browser's dialog methods.
HTMLDialogElement.prototype.showModal = function () {
  this.setAttribute("open", "");
};
HTMLDialogElement.prototype.close = function () {
  this.removeAttribute("open");
};

test("every settings page has its title, and trading connections have their own page", () => {
  const props = {
    snapshot: readySnapshot(),
    refresh: () => {},
    error: "",
    busy: false,
    trade: vi.fn().mockResolvedValue(undefined),
  };
  render(<Settings {...props} initialPage="trading" />);
  expect(screen.getByRole("heading", { level: 1 }).textContent).toBe("交易服务");
  expect(screen.getByRole("button", { name: "添加交易连接" })).toBeTruthy();
  expect(screen.queryByRole("region", { name: "CTP 行情账户" })).toBeNull();
});

test("the two data pages each configure their own service without counter credentials", async () => {
  const snapshot = readySnapshot();
  snapshot.quotes_endpoint = null;
  const trade = vi.fn().mockResolvedValue(undefined);
  render(<Settings snapshot={snapshot} error="" busy={false} trade={trade} initialPage="market" />);
  expect(screen.getByRole("heading", { name: "行情服务", level: 1 })).toBeTruthy();
  expect(screen.queryByRole("heading", { name: "历史数据供应商" })).toBeNull();
  expect(screen.queryByRole("button", { name: "CTP 行情账户" })).toBeNull();
  const market = screen.getByRole("region", { name: "连接" });
  fireEvent.click(within(market).getByRole("button", { name: "添加行情连接" }));
  fireEvent.change(within(market).getByLabelText("主机地址"), {
    target: { value: " 192.168.1.20 " },
  });
  fireEvent.change(within(market).getByLabelText("行情服务证书指纹"), {
    target: { value: "a".repeat(64) },
  });
  fireEvent.click(within(market).getByRole("button", { name: "保存连接" }));
  await waitFor(() =>
    expect(trade).toHaveBeenCalledWith("quotes.endpoint.save", {
      host: "192.168.1.20",
      port: 7451,
      fingerprint: "a".repeat(64),
    }),
  );
  cleanup();
  snapshot.history_endpoint = null;
  render(
    <Settings snapshot={snapshot} error="" busy={false} trade={trade} initialPage="history" />,
  );
  expect(screen.getByRole("heading", { name: "数据服务", level: 1 })).toBeTruthy();
  const history = screen.getByRole("region", { name: "连接" });
  fireEvent.click(within(history).getByRole("button", { name: "添加数据连接" }));
  fireEvent.change(within(history).getByLabelText("主机地址"), { target: { value: "h" } });
  fireEvent.change(within(history).getByLabelText("数据服务证书指纹"), {
    target: { value: "b".repeat(64) },
  });
  fireEvent.click(within(history).getByRole("button", { name: "保存连接" }));
  await waitFor(() =>
    expect(trade).toHaveBeenCalledWith("history.endpoint.save", {
      host: "h",
      port: 7450,
      fingerprint: "b".repeat(64),
    }),
  );
});

test("about identifies the product version and native build commit", () => {
  const snapshot = readySnapshot();
  snapshot.diagnostics.commit = "abc123456789";
  render(
    <Settings snapshot={snapshot} error="" busy={false} trade={vi.fn()} initialPage="about" />,
  );
  expect(screen.getByText(`${version.trim()} (abc123456789)`)).toBeTruthy();
});
