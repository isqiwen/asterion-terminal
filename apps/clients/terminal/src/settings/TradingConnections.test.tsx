import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { readySnapshot } from "../testing/core";
import { TradingConnections } from "./TradingConnections";

// jsdom does not implement the browser's dialog methods.
HTMLDialogElement.prototype.showModal = function () {
  this.setAttribute("open", "");
};
HTMLDialogElement.prototype.close = function () {
  this.removeAttribute("open");
};

test("generates identity explicitly and saves a direct pinned trading destination", async () => {
  const trade = vi.fn().mockResolvedValue(undefined);
  render(<TradingConnections snapshot={readySnapshot()} busy={false} trade={trade} />);
  fireEvent.click(screen.getByRole("button", { name: "生成客户端证书" }));
  await waitFor(() => expect(trade).toHaveBeenCalledWith("trading.identity.prepare", {}));
  fireEvent.click(screen.getByRole("button", { name: "添加交易连接" }));
  for (const [label, value] of [
    ["连接名称", "SimNow"],
    ["交易主机地址", "127.0.0.1"],
    ["端口", "7461"],
    ["交易主机账户实例名", "simnow"],
    ["服务端证书指纹", "a".repeat(64)],
  ])
    fireEvent.change(screen.getByLabelText(label), { target: { value } });
  fireEvent.click(screen.getByRole("button", { name: "保存连接" }));
  await waitFor(() =>
    expect(trade).toHaveBeenCalledWith(
      "trading.connections.save",
      expect.objectContaining({
        name: "SimNow",
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
