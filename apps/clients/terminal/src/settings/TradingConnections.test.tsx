import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { readySnapshot } from "../testing/core";
import { TradingConnections } from "./TradingConnections";

const health = vi.hoisted(() => vi.fn());
vi.mock("../bridge/trading", () => ({ tradingCall: health }));

// jsdom does not implement the browser's dialog methods.
HTMLDialogElement.prototype.showModal = function () {
  this.setAttribute("open", "");
};
HTMLDialogElement.prototype.close = function () {
  this.removeAttribute("open");
};

test("saves a direct pinned trading destination", async () => {
  const trade = vi.fn().mockResolvedValue(undefined);
  render(<TradingConnections snapshot={readySnapshot()} busy={false} trade={trade} />);
  fireEvent.click(screen.getByRole("button", { name: "添加交易连接" }));
  for (const [label, value] of [
    ["主机地址", "127.0.0.1"],
    ["端口", "7461"],
    ["账户", "simnow"],
    ["交易服务证书指纹", "a".repeat(64)],
  ])
    fireEvent.change(screen.getByLabelText(label), { target: { value } });
  fireEvent.click(screen.getByRole("button", { name: "保存连接" }));
  await waitFor(() =>
    expect(trade).toHaveBeenCalledWith(
      "trading.connections.save",
      expect.objectContaining({
        host: "127.0.0.1",
        port: 7461,
        instance: "simnow",
        fingerprint: "a".repeat(64),
        revision: "",
      }),
    ),
  );
  expect(trade.mock.calls.map(call => call[0])).not.toContain("node.deploy");
});

test("a trading connection shows its service's build and a protocol it does not share", async () => {
  health.mockResolvedValue({ version: "0.4.0 (abc1234)", protocol: "theirs" });
  const snapshot = readySnapshot();
  snapshot.trading_protocol = "ours";
  snapshot.trading_connections = [
    {
      id: "simnow",
      host: "10.0.0.2",
      port: 7461,
      instance: "simnow",
      fingerprint: "a".repeat(64),
      revision: "r",
    },
  ];
  render(<TradingConnections snapshot={snapshot} busy={false} trade={vi.fn()} />);
  await screen.findByText("0.4.0 (abc1234)");
  expect(screen.getByText("不一致：请在服务主机安装同一版本")).toBeTruthy();
  expect(health).toHaveBeenCalledWith("simnow", { case: "getHealth", value: {} });
});
