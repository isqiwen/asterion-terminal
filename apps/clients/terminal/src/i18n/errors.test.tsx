import { render, screen } from "@testing-library/react";
import { expect, test } from "vitest";
import { setLocale } from "./index";
import { BackendError, ErrorNotice } from "./errors";

test("protocol mismatch names the service and explains how to recover in both languages", () => {
  const { rerender } = render(
    <ErrorNotice
      error={new BackendError("conflict", "market protocol differs from this Terminal")}
    />,
  );
  expect(screen.getByRole("button").textContent).toBe(
    "行情服务与 Terminal 的通信协议不一致，请将两边更新到同一版本。",
  );
  setLocale("en-US");
  rerender(
    <ErrorNotice
      error={new BackendError("conflict", "market protocol differs from this Terminal")}
    />,
  );
  expect(screen.getByRole("button").textContent).toBe(
    "Market and Terminal use different communication protocols. Update both to the same release.",
  );
});

test("an untranslated failure keeps its cause visible without hovering", () => {
  render(<ErrorNotice error={new BackendError("conflict", "Requested feed is already in use")} />);
  expect(screen.getByRole("button").textContent).toBe(
    "操作与当前状态冲突: Requested feed is already in use",
  );
});

test("unknown wire fields explain protocol mismatch instead of asking the user to fix input", () => {
  const error = new BackendError("invalid_request", "unknown Protobuf field");
  const { rerender } = render(<ErrorNotice error={error} />);
  expect(screen.getByRole("button").textContent).toBe(
    "Terminal 与服务的通信协议不一致，请将两边更新到同一版本。",
  );
  expect(screen.getByRole("button").title).toContain("invalid_request: unknown Protobuf field");
  setLocale("en-US");
  rerender(<ErrorNotice error={error} />);
  expect(screen.getByRole("button").textContent).toBe(
    "Terminal and the service use different communication protocols. Update both to the same release.",
  );
});

test("an inactive catalog front explains availability and automatic recovery in both languages", () => {
  const error = new BackendError(
    "operation_failed",
    "CTP catalog front inactive: CTP catalog authentication rejected: 8: CTP:前置不活跃",
  );
  const { rerender } = render(<ErrorNotice error={error} />);
  expect(screen.getByRole("button").textContent).toContain("柜台前置未就绪（CTP 8），将自动重试");
  expect(screen.getByRole("button").textContent).toContain("交易前置地址和柜台开放时间");
  expect(screen.getByRole("button").title).toContain("CTP:前置不活跃");
  setLocale("en-US");
  rerender(<ErrorNotice error={error} />);
  expect(screen.getByRole("button").textContent).toContain("broker front is inactive (CTP 8)");
  expect(screen.getByRole("button").textContent).toContain("Retrying automatically");
});

test("an unfamiliar catalog refusal keeps the broker's reason and code visible", () => {
  render(
    <ErrorNotice
      error={
        new BackendError("operation_failed", "CTP catalog login rejected: 987: Broker maintenance")
      }
    />,
  );
  expect(screen.getByRole("button").textContent).toBe(
    "合约目录登录失败: Broker maintenance (CTP 987)",
  );
});

test.each([
  ["CTP market front connection timed out", "行情柜台连接超过 30 秒未响应"],
  ["CTP market login timed out", "行情柜台超过 30 秒未回应登录请求"],
])("%s identifies the stalled stage and recovery", (diagnostic, message) => {
  render(<ErrorNotice error={new BackendError("unavailable", diagnostic)} />);
  expect(screen.getByRole("button").textContent).toContain(message);
  expect(screen.getByRole("button").textContent).toContain("将自动重连");
});
