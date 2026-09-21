import { expect, test } from "@playwright/test";
import { nativeContext } from "./native";

test("catalog explains cumulative revisions, gaps and row provenance", async ({
  context,
  page,
}) => {
  await nativeContext(context);
  const type = {
    id: "futures.calendar",
    label: "交易日历",
    domain: "reference",
    domain_label: "基础资料",
    shape: "table",
    frequency: "1d",
    schema_version: 1,
    primary_key: ["exchange", "date"],
    time_semantics: "日历日期",
    description: "合成验收数据",
    fields: [{ name: "date", label: "日期", unit: null }],
  };
  const first = {
    id: "standard-first",
    dataset_id: "calendar",
    job_id: "job-first",
    created_at: 1789600000,
    rows: 1,
    manifest: {
      type,
      source: "tushare",
      layer: "STANDARD",
      scope: { exchange: "SHFE" },
      format: "partition_manifest",
      checksum: "first-checksum",
      bytes: 100,
      inputs: ["raw-first"],
      first: "2024-01-02",
      last: "2024-01-02",
      coverage: "CALENDAR_COMPLETE",
      quality: "VALIDATED",
      version_semantics: "CUMULATIVE",
      parent_version_id: null,
      revision: 1,
      acquired_rows: 1,
      logical_bytes: 1000,
      changes: {
        added: 1,
        revised: 0,
        refreshed: 0,
        unchanged: 0,
        stale_ignored: 0,
      },
      partitions: [
        {
          key: "2024-01",
          first: "2024-01-02",
          last: "2024-01-02",
          rows: 1,
          checksum: "first-month",
        },
      ],
      coverage_gaps: [],
      transform: null,
    },
  };
  const latest = {
    ...first,
    id: "standard-latest",
    job_id: "job-latest",
    rows: 2,
    version_count: 2,
    manifest: {
      ...first.manifest,
      parent_version_id: first.id,
      revision: 2,
      inputs: ["raw-latest", first.id],
      last: "2024-01-04",
      coverage: "CALENDAR_GAPS",
      coverage_gaps: [{ start: "2024-01-03", end: "2024-01-03" }],
      partitions: [
        {
          key: "2024-01",
          first: "2024-01-02",
          last: "2024-01-04",
          rows: 2,
          checksum: "second-month",
        },
      ],
    },
  };
  const raw = {
    ...first,
    id: "raw-first",
    dataset_id: "calendar-raw",
    manifest: {
      ...first.manifest,
      layer: "RAW",
      format: "provider_evidence",
      version_semantics: "ACQUISITION_SCOPE",
      inputs: [],
    },
  };
  await context.route("**/api/v1/**", (route) => {
    if (route.request().url().endsWith("/access/scopes")) return route.fulfill({ json: { token: "scope-fixture", expires: Date.now() / 1000 + 300 } });

    const path = new URL(route.request().url()).pathname;
    if (path.includes("/account/security")) return route.fallback();
    if (path.endsWith("/data/types")) return route.fulfill({ json: [type] });
    if (path.endsWith("/data/catalog"))
      return route.fulfill({ json: { items: [latest], total: 1, offset: 0 } });
    if (path.endsWith("/versions"))
      return route.fulfill({
        json: {
          items: path.includes("calendar-raw") ? [raw] : [latest, first],
          total: 2,
          offset: 0,
        },
      });
    if (path.includes("/data/versions/")) {
      const version = path.endsWith(raw.id)
        ? raw
        : path.endsWith(first.id)
          ? first
          : latest;
      return route.fulfill({
        json: {
          version,
          rows:
            version === raw
              ? [{ cal_date: "20240102" }]
              : version === first
                ? [{ date: "2024-01-02" }]
                : [{ date: "2024-01-02" }, { date: "2024-01-04" }],
          total: version.rows,
          offset: 0,
          snapshot: null,
          row_sources:
            version === raw
              ? null
              : Array.from({ length: version.rows }, () => ({
                  observed_at: "2026-09-19T01:00:00Z",
                  raw_version_id: raw.id,
                })),
        },
      });
    }
    const user = {
      email: "cumulative@example.com",
      first_name: "Cumulative",
      last_name: "Test",
    };
    return route.fulfill({
      json: path.endsWith("/login")
        ? { session: "session", user }
        : path.endsWith("/me")
          ? user
          : path.endsWith("/health")
            ? { status: "ready" }
            : [],
    });
  });
  await page.goto("/");
  await page.getByLabel("邮箱", { exact: true }).fill("cumulative@example.com");
  await page
    .getByLabel("密码", { exact: true })
    .fill("synthetic-test-password");
  await page.getByRole("button", { name: "登录", exact: true }).click();
  await page
    .getByRole("navigation", { name: "业务工作区" })
    .getByRole("button", { name: "数据", exact: true })
    .click();
  await page.getByRole("button", { name: "数据集", exact: true }).click();
  await page.getByRole("button", { name: "交易日历", exact: true }).click();
  await expect(page.getByText("累积版本 #2", { exact: false })).toBeVisible();
  await expect(page.getByRole("status")).toContainText(
    "未覆盖的日历日期：2024-01-03 — 2024-01-03",
  );
  await expect(page.getByRole("table", { name: "版本分区" })).toContainText(
    "2024-01",
  );
  await page.getByRole("button", { name: "查看父版本", exact: true }).click();
  await expect(page.getByLabel("采集版本")).toHaveValue(first.id);
  await expect(page.getByText("累积版本 #1", { exact: false })).toBeVisible();
  await page.getByLabel("采集版本").selectOption(latest.id);
  await page.screenshot({ path: "../../.state/cumulative-catalog.png" });
  await page.getByRole("button", { name: "查看该行来源" }).first().click();
  await expect(
    page.getByRole("heading", { name: "交易日历 · 原始预览" }),
  ).toBeVisible();
  await expect(
    page.getByRole("cell", { name: "20240102", exact: true }),
  ).toBeVisible();
});
