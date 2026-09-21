import { coverageIdentity } from "./reference-fixture";
import { expect, test } from "@playwright/test";
import { nativeContext } from "./native";

test("daily coverage pins evidence, excludes unknown dates and submits idempotent refills", async ({
  context,
  page,
}) => {
  await nativeContext(context);
  let hasReferences = false,
    filled = false,
    attempts = 0;
  let stored: Record<string, unknown> | null = null;
  const commands: string[] = [];
  let archived = false, revision = 0, archiveConflict = false;
  const checks: Record<string, unknown>[] = [];
  const type = {
    id: "futures.daily",
    label: "历史日线",
    domain: "market",
    domain_label: "行情",
    shape: "timeseries",
    frequency: "1d",
    schema_version: 1,
    primary_key: ["contract", "trading_day"],
    fields: [{ name: "trading_day", label: "交易日", unit: null }],
    time_semantics: "交易日标签",
    description: "合成覆盖验收",
  };
  const version = {
    id: "daily-fixture",
    dataset_id: "daily-dataset",
    job_id: "job-original",
    created_at: 1789600000,
    rows: 1,
    version_count: 1,
    manifest: {
      type,
      layer: "STANDARD",
      source: "tushare",
      scope: { exchange: "SHFE", symbol: "RB2610.SHF" },
      first: "2024-01-02",
      last: "2024-01-04",
      inputs: [],
      format: "partition_manifest",
      checksum: "synthetic",
      bytes: 123,
      logical_bytes: 1000,
      quality: "VALIDATED",
      coverage: "RETURNED_ROWS_ONLY",
      version_semantics: "CUMULATIVE",
      revision: 1,
      acquired_rows: 1,
      partitions: [],
      transform: null,
    },
  };
  const job = {
    id: "fill-job",
    command_id: "fixture",
    kind: "data.sync",
    state: "QUEUED",
    attempt: 0,
    created_at: 1789600001,
    error: null,
    result: null,
  };
  function report() {
    return {
      id: hasReferences
        ? filled
          ? "report-filled"
          : "report-gaps"
        : "report-unknown",
      checker: "daily-coverage-v2",
          identity: coverageIdentity(), reference_symbol: null,
      created_at: 1789600000,
      as_of: "2026-09-19",
      daily_version_id: version.id,
      calendar_version_id: hasReferences ? "calendar-fixed" : null,
      contracts_version_id: hasReferences ? "contracts-fixed" : null,
      source: "tushare",
      exchange: "SHFE",
      symbol: "RB2610.SHF",
      start: "2024-01-02",
      end: "2024-01-04",
      status: !hasReferences ? "UNCONFIRMED" : filled ? "COVERED" : "GAPS",
      counts: !hasReferences
        ? { UNKNOWN_CONTRACT: 3 }
        : filled
          ? { PRESENT: 2, CLOSED: 1 }
          : { PRESENT: 1, GAP: 1, CLOSED: 1 },
      days: [2, 3, 4].map((day) => ({
        date: `2024-01-0${day}`,
        status: !hasReferences
          ? "UNKNOWN_CONTRACT"
          : day === 3
            ? "CLOSED"
            : day === 2 || filled
              ? "PRESENT"
              : "GAP",
        has_data: day === 2 || (filled && day === 4),
        reason: "合成依据说明",
      })),
      refill_ranges:
        hasReferences && !filled
          ? [{ start: "2024-01-04", end: "2024-01-04" }]
          : [],
      notes: ["结论仅适用于列出的行情、交易日历及合约资料版本。"],
    };
  }
  await context.route("**/api/v1/**", (route) => {
    if (route.request().url().endsWith("/access/scopes")) return route.fulfill({ json: { token: "scope-fixture", expires: Date.now() / 1000 + 300 } });

    const path = new URL(route.request().url()).pathname;
    if (path.includes("/account/security")) return route.fallback();
    if (path.endsWith("/coverage")) {
      if (route.request().method() === "POST") {
        checks.push(route.request().postDataJSON());
        stored = report();
      }
      return route.fulfill({ json: stored });
    }
    if (path.endsWith("/refill")) {
      commands.push(route.request().postDataJSON().command_id);
      attempts++;
      if (attempts === 1)
        return route.fulfill({
          status: 503,
          json: { detail: "合成：响应暂不可用，请重试" },
        });
      return route.fulfill({
        json: {
          status: "QUEUED",
          report_id: "report-gaps",
          checked_report_id: "report-gaps",
          jobs: [job],
          skipped_gap_days: 0,
        },
      });
    }
    if (path.endsWith("/lifecycle") || path.endsWith("/archive")) {
      if (path.endsWith("/archive")) {
        if (archiveConflict) return route.fulfill({status:409,json:{detail:"版本归档状态已被另一窗口修改"}});
        const body = route.request().postDataJSON();
        expect(body.expected_revision).toBe(revision);
        archived = body.archived; revision++;
      }
      return route.fulfill({json:{version_id:version.id,archived,revision,is_latest:true,references:{research_runs:1,coverage_reports:2},reference_count:3,protection_reason:"所有版本保留文件与固定引用；暂不提供永久删除或分区回收。"}});
    }
    if (path.endsWith("/data/types")) return route.fulfill({ json: [type] });
    if (path.endsWith("/data/catalog")) {
      const visible = !archived || new URL(route.request().url()).searchParams.get("include_archived") === "true";
      return route.fulfill({json:{items:visible ? [{...version,archived}] : [],total:visible ? 1 : 0,offset:0}});
    }
    if (path.endsWith("/versions"))
      return route.fulfill({ json: { items: [version], total: 1, offset: 0 } });
    if (path.includes("/data/versions/"))
      return route.fulfill({
        json: {
          version,
          rows: [{ trading_day: "2024-01-02" }],
          total: 1,
          offset: 0,
          snapshot: null,
        },
      });
    const user = {
      email: "coverage@example.com",
      first_name: "Coverage",
      last_name: "Test",
    };
    return route.fulfill({
      json: path.endsWith("/jobs")
        ? attempts > 1
          ? [job]
          : []
        : path.endsWith("/login")
          ? { session: "session", user }
          : path.endsWith("/me")
            ? user
            : path.endsWith("/health")
              ? { status: "ready" }
              : [],
    });
  });
  await page.goto("/");
  await page.getByLabel("邮箱", { exact: true }).fill("coverage@example.com");
  await page
    .getByLabel("密码", { exact: true })
    .fill("synthetic-test-password");
  await page.getByRole("button", { name: "登录", exact: true }).click();
  await page
    .getByRole("navigation", { name: "业务工作区" })
    .getByRole("button", { name: "数据", exact: true })
    .click();
  await page.getByRole("button", { name: "数据集", exact: true }).click();
  await page.getByRole("button", { name: "历史日线", exact: true }).click();
  await page.getByText("日线覆盖核对与补齐", { exact: true }).click();
  await page.getByRole("button", { name: "核对选中版本", exact: true }).click();
  await expect(
    page.getByText("部分日期无法确认", { exact: false }),
  ).toBeVisible();
  await expect(
    page.getByRole("button", { name: "补齐确认缺口", exact: false }),
  ).toBeDisabled();
  await expect(page.getByText("缺少同源交易日历")).toBeVisible();
  hasReferences = true;
  await page
    .getByRole("button", { name: "核对最新行情版本", exact: true })
    .click();
  await expect(
    page.getByRole("button", { name: "补齐确认缺口（1 个区间）" }),
  ).toBeEnabled();
  expect(checks.at(-1)).toMatchObject({
    use_latest_daily: true,
    start: "2024-01-02",
    end: "2024-01-04",
  });
  await page.getByLabel("覆盖日期状态").selectOption("GAP");
  await expect(
    page.getByRole("table", { name: "日线覆盖逐日结果" }),
  ).toContainText("2024-01-04");
  await expect(
    page.getByRole("table", { name: "日线覆盖逐日结果" }),
  ).not.toContainText("2024-01-03");
  await page.getByRole("button", { name: "补齐确认缺口（1 个区间）" }).click();
  await expect(page.getByRole("alert")).toContainText("响应暂不可用");
  await page.getByRole("button", { name: "补齐确认缺口（1 个区间）" }).click();
  await expect(
    page.getByText("已提交 1 项补齐任务", { exact: false }),
  ).toBeVisible();
  expect(commands[0]).toBe(commands[1]);
  await expect(page.getByRole("region", { name: "任务中心" })).toBeVisible();
  filled = true;
  await page
    .getByRole("button", { name: "核对最新行情版本", exact: true })
    .click();
  await expect(
    page.getByText("所选依据下记录齐全", { exact: false }),
  ).toBeVisible();
  await expect(
    page.getByRole("button", { name: "补齐确认缺口（0 个区间）" }),
  ).toBeDisabled();
  await page.getByRole("button", { name: "收起任务中心" }).click();
  await page.getByText("引用保护与归档",{exact:true}).click();
  await expect(page.getByText("研究运行（含排队与失败）：1",{exact:true})).toBeVisible();
  await page.getByRole("button",{name:"归档此版本",exact:true}).click();
  await expect(page.getByText("已归档",{exact:true})).toBeVisible();
  await expect(page.getByText(/没有符合条件的数据集/)).toBeVisible();
  await page.getByRole("checkbox",{name:"显示归档版本",exact:true}).check();
  await page.getByRole("button",{name:"历史日线",exact:true}).click();
  await page.getByText("引用保护与归档",{exact:true}).click();
  archiveConflict = true;
  await page.getByRole("button",{name:"恢复此版本",exact:true}).click();
  await expect(page.getByRole("alert")).toContainText("另一窗口");
  archiveConflict = false;
  await page.getByRole("button",{name:"刷新引用与状态",exact:true}).click();
  await page.getByRole("button",{name:"恢复此版本",exact:true}).click();
  await expect(page.getByText("未归档",{exact:true})).toBeVisible();
  await page.screenshot({ path: "../../.state/daily-coverage.png" });
});
