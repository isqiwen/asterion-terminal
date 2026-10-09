import { fireEvent, render, screen, within } from "@testing-library/react";
import { expect, test } from "vitest";
import type { Snapshot } from "../../bridge/client";
import { localHealth, localService, readySnapshot } from "../../testing/core";
import { ServiceStatus } from "./ServiceStatus";

const show = (snapshot: Snapshot) => (
  <ServiceStatus
    snapshot={snapshot}
    failed={false}
    busy={false}
    checkedAt={null}
    refresh={() => {}}
    settings={() => {}}
  />
);
const indicator = () => screen.getByRole("button", { name: "查看服务连接" });

test("a service that has just started reads as starting, not as a fault", () => {
  const report = (health: string) => {
    const snapshot = readySnapshot();
    for (const service of localHealth(snapshot).services) {
      service.health = health;
      // As the Agent reports it: the missed first probe is the error text.
      service.error = health === "ready" ? "" : "IPC endpoint is not ready";
    }
    return show(snapshot);
  };
  const view = render(report("ready"));
  expect(indicator().textContent).toBe("● 服务");
  expect(indicator().className).toBe("good");
  view.rerender(report("starting"));
  expect(indicator().textContent).toBe("● 服务启动中");
  expect(indicator().className).toBe("");
  view.rerender(report("unresponsive"));
  expect(indicator().textContent).toBe("● 服务异常");
  expect(indicator().className).toBe("bad");
});

test("log loss stays visible while a failed background refresh can recover", () => {
  const report = (refreshFailed: boolean, logFailures: number) => {
    const snapshot = readySnapshot();
    snapshot.diagnostics.log_failures = logFailures;
    snapshot.diagnostics.refresh_failed = refreshFailed;
    snapshot.diagnostics.refresh_failures = refreshFailed ? 1 : 0;
    return show(snapshot);
  };
  const view = render(report(false, 0));
  expect(indicator().className).toBe("good");
  view.rerender(report(true, 3));
  expect(indicator().textContent).toBe("● 服务待确认");
  fireEvent.click(indicator());
  const panel = screen.getByRole("region", { name: "服务连接详情" });
  expect(panel.textContent).toContain("后台状态刷新失败");
  expect(panel.textContent).toContain("本次运行有日志写入失败");
  view.rerender(report(false, 3));
  expect(panel.textContent).not.toContain("后台状态刷新失败");
  expect(panel.textContent).toContain("本次运行有日志写入失败");
  expect(indicator().textContent).toBe("● 诊断不完整");
  expect(indicator().className).toBe("bad");
});

test("old account progress stays uncertain even while transport responds", () => {
  const report = (stale: boolean) => {
    const snapshot = readySnapshot();
    const account = { ...localService(snapshot, "task"), id: "live-1", kind: "live" as const };
    account.health = stale ? "degraded" : "ready";
    account.execution = {
      ...account.execution!,
      state: { observed: true, pending: stale, age_ms: stale ? 60000 : 0 },
      persistence: { observed: true, pending: false, age_ms: 60000 },
    };
    localHealth(snapshot).services.push(account);
    return show(snapshot);
  };
  const view = render(report(false));
  fireEvent.click(indicator());
  // The account's own block of the connection details.
  const row = (label: string) =>
    within(screen.getByText("live-1").parentElement!.parentElement!).getByText(label).parentElement!
      .textContent;
  expect(row("业务就绪")).toContain("已就绪");
  expect(row("持久化与准备")).toContain("空闲");
  view.rerender(report(true));
  expect(row("状态推进")).toContain("进展超时");
  expect(row("业务就绪")).toContain("待确认");
  expect(row("请求处理")).toContain("正常");
});
