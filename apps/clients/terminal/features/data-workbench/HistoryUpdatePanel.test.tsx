import { fireEvent, render, screen } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { readySnapshot } from "../../src/testing/core";
import type { DailyTable, HistoryDatasetRecord, HistoryUpdatePlan } from "../../src/bridge/client";
import { HistoryUpdatePanel } from "./HistoryUpdatePanel";
import { feature } from "./feature";
registerTerminalFeatures([feature]);
const daily: DailyTable = {
  snapshot: "fixed-reference",
  daily_source: "daily",
  calendar_source: "calendar",
  contracts_source: "contracts",
};
const item: HistoryDatasetRecord = {
  id: "minute",
  contract_id: "SHFE/rb/2026-10",
  interval_minutes: 1,
  source: "tushare.ft_mins",
  revision: "fixed",
  begin: "2026-09-23 09:00:00",
  end: "2026-09-25 09:00:00",
  rows: 2,
};
const plan: HistoryUpdatePlan = {
  id: "plan",
  query: { dataset_id: item.id, daily, mode: "repair", end_day: "", requests_per_minute: 60 },
  source: item.source,
  contract_id: item.contract_id,
  interval_minutes: 1,
  begin: item.begin,
  end: item.end,
  missing_days: [],
  session_rules_version: 1,
  conflicts: [
    {
      day: "2026-09-25",
      observed_minutes: true,
      calendar_open: true,
      daily_auction: true,
      closure: {
        rule_id: "SHFE.2026.mid-autumn-national.closures",
        version: 1,
        exchange: "SHFE",
        first_day: "2026-09-25",
        last_day: "2026-09-27",
        published_day: "2026-09-21",
        source_url: "https://www.shfe.com.cn/publicnotice/notice/202609/t20260921_833503.html",
      },
    },
  ],
};
function context(result: HistoryUpdatePlan) {
  const snapshot = readySnapshot();
  snapshot.data!.sources = [
    {
      id: item.source,
      name: "Fixture",
      plugin_id: "fixture",
      normalization: "fixture.v1",
      timezone: "Asia/Shanghai",
      timestamp_semantics: "bar_end",
      venues: ["SHFE"],
      intervals: [1],
      max_requests_per_minute: 60,
      credential_required: true,
    },
  ];
  return {
    snapshot,
    busy: false,
    error: "",
    navigate: vi.fn(),
    openSettings: vi.fn(),
    trade: vi.fn(async () => {}),
    query: vi.fn(async () => ({ ...snapshot, history_update_plan: result })),
  };
}
test("blocked repair explains dates, sources and next steps without credential, quota or submit controls", async () => {
  const ctx = context(plan);
  const close = vi.fn();
  render(<HistoryUpdatePanel context={ctx} item={item} daily={daily} onClose={close} />);
  fireEvent.click(screen.getByRole("button", { name: "预览下载范围" }));
  const alert = await screen.findByRole("alert");
  expect(alert.textContent).toContain("2026-09-25");
  expect(alert.textContent).toContain("所选日历标记开市");
  expect(alert.textContent).toContain("fixed-reference");
  expect(alert.textContent).toContain("重新打开历史数据仓库");
  expect(screen.getByRole("link").getAttribute("href")).toBe(plan.conflicts[0].closure.source_url);
  expect(screen.queryByRole("button", { name: "开始下载此范围" })).toBeNull();
  expect(screen.queryByRole("button", { name: "应用共享额度" })).toBeNull();
  expect(screen.queryByLabelText("数据源凭据")).toBeNull();
  expect(ctx.trade).not.toHaveBeenCalled();
  fireEvent.click(screen.getByRole("button", { name: "关闭预览并检查参考" }));
  expect(close).toHaveBeenCalledOnce();
});
test("unblocked whole-day preview retains its explicit manual action and unknown-session caveat", async () => {
  const ctx = context({ ...plan, conflicts: [], missing_days: ["2026-09-24"] });
  render(<HistoryUpdatePanel context={ctx} item={item} daily={daily} onClose={() => {}} />);
  fireEvent.click(screen.getByRole("button", { name: "预览下载范围" }));
  await screen.findByText("待核对 1 个交易日：2026-09-24");
  expect(screen.getByText(/其他日期的历史时段仍未知/)).toBeTruthy();
  expect(
    (screen.getByRole("button", { name: "开始下载此范围" }) as HTMLButtonElement).disabled,
  ).toBe(true);
  expect(ctx.trade).not.toHaveBeenCalled();
});

test("Data can preview and execute with Research offline", async () => {
  const ctx = context({ ...plan, conflicts: [], missing_days: ["2026-09-24"] });
  ctx.snapshot.research_service!.online = false;
  ctx.snapshot.data!.sources[0].credential_required = false;
  const view = render(
    <HistoryUpdatePanel context={ctx} item={item} daily={daily} onClose={() => {}} />,
  );
  const preview = screen.getByRole("button", { name: "预览下载范围" });
  expect((preview.closest("fieldset") as HTMLFieldSetElement).disabled).toBe(false);
  fireEvent.click(preview);
  await screen.findByText("待核对 1 个交易日：2026-09-24");
  const submit = () => screen.getByRole("button", { name: "开始下载此范围" }) as HTMLButtonElement;
  expect(submit().disabled).toBe(false);
  expect(ctx.trade).not.toHaveBeenCalled();
  ctx.snapshot = {
    ...ctx.snapshot,
    research_service: { ...ctx.snapshot.research_service!, online: true },
  };
  view.rerender(<HistoryUpdatePanel context={ctx} item={item} daily={daily} onClose={() => {}} />);
  expect(submit().disabled).toBe(false);
  expect(ctx.query).toHaveBeenCalledTimes(1);
  expect(ctx.trade).not.toHaveBeenCalled();
});
