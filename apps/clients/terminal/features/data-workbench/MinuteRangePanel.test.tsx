import { act, fireEvent, render, screen, within } from "@testing-library/react";
import { expect, test, vi } from "vitest";
import type { DailyTable, MinuteRangeEvidence, Snapshot } from "../../src/bridge/client";
import { registerTerminalFeatures } from "../../src/host/feature-registry";
import { readySnapshot } from "../../src/testing/core";
import { MinuteRangePanel } from "./MinuteRangePanel";
import { feature } from "./feature";
registerTerminalFeatures([feature]);
const daily: DailyTable = {
  snapshot: "fixed",
  daily_source: "daily",
  calendar_source: "calendar",
  contracts_source: "contracts",
};
const evidence: MinuteRangeEvidence = {
  session_rules_version: 1,
  dataset_id: "minute",
  daily,
  begin_day: "2026-09-04",
  end_day: "2026-09-07",
  contract_id: "SHFE/rb/2026-10",
  minute_source: "source",
  interval_minutes: 1,
  request_begin_ns: "1788537600000000000",
  request_end_ns: "1788796800000000000",
  unknown_rows: 1,
  request_session_coverage: "unverified",
  days: [
    {
      trading_day: "2026-09-04",
      state: "RANGE_UNKNOWN_LABELS",
      observed: null,
      official_closure: null,
      evening_closure: null,
    },
    {
      trading_day: "2026-09-05",
      state: "RANGE_CLOSED",
      observed: null,
      official_closure: null,
      evening_closure: null,
    },
    {
      trading_day: "2026-09-06",
      state: "RANGE_UNKNOWN_CALENDAR",
      observed: null,
      official_closure: null,
      evening_closure: null,
    },
    {
      trading_day: "2026-09-07",
      state: "RANGE_OBSERVED",
      official_closure: null,
      evening_closure: null,
      observed: { rows: 2, first_ns: "1788537600000000000", last_ns: "1788796800000000000" },
    },
  ],
};
function dates() {
  fireEvent.change(screen.getByLabelText("目标起始交易日"), { target: { value: "2026-09-04" } });
  fireEvent.change(screen.getByLabelText("目标结束交易日"), { target: { value: "2026-09-07" } });
}
test("audits explicit dates against a fixed reference and preserves missing observations", async () => {
  const query = vi.fn(async () => ({ ...readySnapshot(), minute_range: evidence }));
  render(<MinuteRangePanel id="minute" daily={daily} query={query} disabled={false} />);
  expect(query).not.toHaveBeenCalled();
  dates();
  fireEvent.click(screen.getByRole("button", { name: "核对目标范围" }));
  await screen.findByRole("table");
  expect(query).toHaveBeenCalledWith("data.minutes.range", {
    dataset_id: "minute",
    daily,
    begin_day: evidence.begin_day,
    end_day: evidence.end_day,
    session_rules_version: 1,
  });
  const rows = screen.getByRole("table").querySelectorAll("tbody tr");
  expect(rows).toHaveLength(4);
  expect(within(rows[0] as HTMLElement).getAllByText("—")).toHaveLength(3);
  expect(rows[0].textContent).toContain("存在无法归属交易日的记录");
  expect(rows[1].textContent).toContain("日历标记休市");
  expect(rows[2].textContent).toContain("日历未知");
  expect(rows[3].textContent).toContain("已观察到分钟记录");
  expect(screen.getByText(/目标范围独立于原始下载时段/).textContent).toContain("未验证");
  fireEvent.change(screen.getByLabelText("目标结束交易日"), { target: { value: "2026-09-08" } });
  expect(screen.queryByRole("table")).toBeNull();
});
test("requires a fixed daily reference and rejects a mismatched snapshot", async () => {
  const query = vi.fn(async () => ({
    ...readySnapshot(),
    minute_range: { ...evidence, daily: { ...daily, snapshot: "other" } },
  }));
  const view = render(<MinuteRangePanel id="minute" daily={null} query={query} disabled={false} />);
  dates();
  expect((screen.getByRole("button", { name: "核对目标范围" }) as HTMLButtonElement).disabled).toBe(
    true,
  );
  expect(query).not.toHaveBeenCalled();
  view.rerender(<MinuteRangePanel id="minute" daily={daily} query={query} disabled={false} />);
  fireEvent.click(screen.getByRole("button", { name: "核对目标范围" }));
  await screen.findByRole("alert");
  expect(screen.queryByRole("table")).toBeNull();
});
test("a stale service reply cannot populate its replacement", async () => {
  let release!: (value: Snapshot) => void;
  const pending = new Promise<Snapshot>(resolve => {
    release = resolve;
  });
  const view = render(
    <MinuteRangePanel
      key="old"
      id="minute"
      daily={daily}
      query={async () => pending}
      disabled={false}
    />,
  );
  dates();
  fireEvent.click(screen.getByRole("button", { name: "核对目标范围" }));
  view.rerender(
    <MinuteRangePanel
      key="new"
      id="minute"
      daily={daily}
      query={async () => readySnapshot()}
      disabled={false}
    />,
  );
  await act(async () => release({ ...readySnapshot(), minute_range: evidence }));
  expect(screen.queryByRole("table")).toBeNull();
});

test("shows bounded official closure references separately from evening civil-date cancellations", async () => {
  const source = {
    rule_id: "fixture.sessions.closure",
    version: 1,
    exchange: "SHFE",
    first_day: "2026-09-04",
    last_day: "2026-09-04",
    published_day: "2026-09-01",
    source_url: "https://example.invalid/fixture-closure",
  };
  const page: MinuteRangeEvidence = {
    ...evidence,
    days: [
      {
        ...evidence.days[0],
        state: "RANGE_RULE_CONFLICT",
        official_closure: source,
        evening_closure: source,
      },
    ],
  };
  const query = vi.fn(async () => ({ ...readySnapshot(), minute_range: page }));
  render(<MinuteRangePanel id="minute" daily={daily} query={query} disabled={false} />);
  dates();
  fireEvent.click(screen.getByRole("button", { name: "核对目标范围" }));
  await screen.findByRole("table");
  expect(screen.getByText("本地观测或参考与官方休市公告冲突")).toBeTruthy();
  expect(screen.getByText(/该自然日晚间停夜盘/)).toBeTruthy();
  expect(screen.getAllByRole("link")[0].getAttribute("href")).toBe(source.source_url);
  expect(screen.getByText(/发布日期/).textContent).toContain("2026-09-01");
});
