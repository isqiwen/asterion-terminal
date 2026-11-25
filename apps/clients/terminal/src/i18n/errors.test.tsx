import { render, screen } from "@testing-library/react";
import { expect, test } from "vitest";
import { setLocale } from "./index";
import { BackendError, ErrorNotice, diagnosticSummary, prefixedDiagnostics } from "./errors";

test("protocol mismatch names the service and explains how to recover in both languages", () => {
  const { container, rerender } = render(
    <ErrorNotice
      error={new BackendError("conflict", "market protocol differs from this Terminal")}
    />,
  );
  const notice = () => container.querySelector(".error-notice") as HTMLElement;
  expect(screen.queryByRole("button")).toBeNull();
  expect(notice().textContent).toBe(
    "行情服务与 Terminal 的通信协议不一致，请将两边更新到同一版本。",
  );
  setLocale("en-US");
  rerender(
    <ErrorNotice
      error={new BackendError("conflict", "market protocol differs from this Terminal")}
    />,
  );
  expect(notice().textContent).toBe(
    "Market and Terminal use different communication protocols. Update both to the same release.",
  );
});

test("an untranslated failure keeps its cause visible without hovering", () => {
  const { container } = render(
    <ErrorNotice error={new BackendError("conflict", "Requested feed is already in use")} />,
  );
  expect(container.querySelector(".error-notice")!.textContent).toBe(
    "操作与当前状态冲突: Requested feed is already in use",
  );
});

test("unknown wire fields explain protocol mismatch instead of asking the user to fix input", () => {
  const error = new BackendError("invalid_request", "unknown Protobuf field");
  const { container, rerender } = render(<ErrorNotice error={error} />);
  const notice = () => container.querySelector(".error-notice") as HTMLElement;
  expect(notice().textContent).toBe("Terminal 与服务的通信协议不一致，请将两边更新到同一版本。");
  expect(notice().title).toBe("invalid_request: unknown Protobuf field");
  setLocale("en-US");
  rerender(<ErrorNotice error={error} />);
  expect(notice().textContent).toBe(
    "Terminal and the service use different communication protocols. Update both to the same release.",
  );
});

test("an inactive or uninitialized catalog front explains availability and automatic recovery in both languages", () => {
  const uninitialized = new BackendError(
    "unavailable",
    "CTP catalog not initialized: CTP catalog authentication rejected: 7: CTP:还没有初始化",
  );
  const error = new BackendError(
    "operation_failed",
    "CTP catalog front inactive: CTP catalog authentication rejected: 8: CTP:前置不活跃",
  );
  const { container, rerender } = render(<ErrorNotice error={uninitialized} />);
  const notice = () => container.querySelector(".error-notice") as HTMLElement;
  expect(notice().textContent).toContain("柜台尚未初始化完成（CTP 7），将自动重试");
  expect(notice().title).toContain("CTP:还没有初始化");
  rerender(<ErrorNotice error={error} />);
  expect(notice().textContent).toContain("柜台前置未就绪（CTP 8），将自动重试");
  expect(notice().textContent).toContain("交易前置地址和柜台开放时间");
  expect(notice().title).toContain("CTP:前置不活跃");
  setLocale("en-US");
  rerender(<ErrorNotice error={uninitialized} />);
  expect(notice().textContent).toContain("broker is not initialized yet (CTP 7)");
  rerender(<ErrorNotice error={error} />);
  expect(notice().textContent).toContain("broker front is inactive (CTP 8)");
  expect(notice().textContent).toContain("Retrying automatically");
});

test("an unfamiliar catalog refusal keeps the broker's reason and code visible", () => {
  const { container } = render(
    <ErrorNotice
      error={
        new BackendError("operation_failed", "CTP catalog login rejected: 987: Broker maintenance")
      }
    />,
  );
  expect(container.querySelector(".error-notice")!.textContent).toBe(
    "合约目录登录失败: Broker maintenance (CTP 987)",
  );
});

test.each([
  ["CTP market front connection timed out", "行情服务已连接，但行情柜台超过 30 秒未响应"],
  ["CTP market login timed out", "行情服务已连接，但行情柜台超过 30 秒未回应登录请求"],
])("%s identifies the stalled stage and recovery", (diagnostic, message) => {
  setLocale("zh-CN");
  const { container } = render(<ErrorNotice error={new BackendError("unavailable", diagnostic)} />);
  const text = container.querySelector(".error-notice")!.textContent;
  expect(text).toContain(message);
  expect(text).toContain("将自动重连");
});

test("known UI notices stay plain text without raw diagnostic copy buttons in English", () => {
  setLocale("en-US");
  render(<ErrorNotice error="保存失败，请检查本机存储权限。" />);
  expect(screen.queryByRole("button")).toBeNull();
  expect(screen.getByText("Save failed. Check local storage permissions.")).not.toBeNull();
  setLocale("zh-CN");
});

test("every prefixed diagnostic resolves cleanly in both locales without throwing", () => {
  for (const locale of ["zh-CN", "en-US"] as const) {
    setLocale(locale);
    for (const [prefix, , includeDetail] of prefixedDiagnostics) {
      const summary = diagnosticSummary(`${prefix}sample-detail`);
      expect(summary, `${locale}: ${prefix}`).toBeTruthy();
      expect(summary).not.toContain("全市场数据");
      if (includeDetail !== false)
        expect(summary, `${locale}: ${prefix}`).toContain("sample-detail");
    }
  }
  setLocale("zh-CN");
});

test("RPC, stream, TLS and Lab schema failures state exact causes in both languages", () => {
  const cases = [
    ["RPC connection refused", "服务拒绝连接，请求未发送", "Connection refused by the service"],
    [
      "RPC server certificate fingerprint mismatch",
      "服务端证书指纹与设置不匹配",
      "Server certificate fingerprint does not match",
    ],
    [
      "RPC client certificate rejected by server",
      "服务端拒绝了本机客户端证书",
      "rejected this client's certificate",
    ],
    [
      "RPC stream timed out waiting for next frame",
      "实时推送流超过 5 秒未收到新数据，正在自动重连",
      "Live stream received no data for over 5 seconds",
    ],
    ["RPC read timed out", "查询服务超时", "Query timed out waiting for the service"],
    [
      "RPC request timed out; command outcome may be unknown",
      "命令是否已生效尚不确定",
      "command outcome may be unknown",
    ],
    [
      "lab runs database schema is outdated: /tmp/runs.sqlite",
      "研究运行索引数据库表结构已过期",
      "Lab runs index database schema is outdated",
    ],
  ] as const;
  for (const [diagnostic, zh, en] of cases) {
    setLocale("zh-CN");
    const zhText = diagnosticSummary(diagnostic);
    expect(zhText).toContain(zh);
    if (diagnostic.includes("/tmp/runs.sqlite")) expect(zhText).toContain("/tmp/runs.sqlite");
    setLocale("en-US");
    expect(diagnosticSummary(diagnostic)).toContain(en);
  }
  setLocale("zh-CN");
});

test("prefixed evidence and risk breaches stay visible without raw punctuation joins", () => {
  setLocale("zh-CN");
  const { container, rerender } = render(
    <ErrorNotice
      error={new BackendError("operation_failed", "Tushare reported an error: 超过每分钟请求上限")}
    />,
  );
  const notice = () => container.querySelector(".error-notice") as HTMLElement;
  expect(notice().textContent).toBe("Tushare 返回了错误: 超过每分钟请求上限");

  rerender(<ErrorNotice error={new BackendError("risk_breached", "acct1: daily_loss")} />);
  expect(notice().textContent).toBe("账户触发硬风控，已暂停新增开仓: acct1 (日内亏损达限)");

  rerender(
    <ErrorNotice
      error={new BackendError("risk_breached", "acct1: daily_cancels:SHFE/rb/2026-05")}
    />,
  );
  expect(notice().textContent).toBe(
    "账户触发硬风控，已暂停新增开仓: acct1 (SHFE/rb/2026-05 日撤单达限)",
  );

  setLocale("en-US");
  rerender(<ErrorNotice error={new BackendError("risk_breached", "acct1: daily_loss")} />);
  expect(notice().textContent).toBe(
    "Hard risk limit triggered on an account; new opening orders are paused: acct1 (Daily loss limit reached)",
  );
  setLocale("zh-CN");
});
