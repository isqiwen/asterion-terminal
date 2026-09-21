import { coverageIdentity } from "./reference-fixture";
import { strategies } from "./strategy-fixture";
import { rulesRoute, selectRules } from "./rules-fixture";
import { test, expect } from "@playwright/test";
import { nativeContext } from "./native";

for (const lostResponse of [false, true]) {
  test(`research refill restart, retry and explicit adoption (lostResponse=${lostResponse})`, async ({
    context,
    page,
  }) => {
    await nativeContext(context);
    const type = {
      id: "futures.daily",
      label: "日线",
      domain: "market",
      domain_label: "行情",
      shape: "timeseries",
      frequency: "1d",
      schema_version: 1,
      primary_key: ["contract", "trading_day"],
      fields: [],
      time_semantics: "交易日",
      description: "测试",
    };
    const old = {
      id: "daily-old",
      dataset_id: "dataset-one",
      job_id: "source-job",
      created_at: 1,
      rows: 25,
      manifest: {
        type,
        source: "tushare",
        layer: "STANDARD",
        scope: { contract: "SHFE.rb2405" },
        first: "2024-01-01",
        last: "2024-01-31",
        inputs: [],
        format: "partition_manifest",
        checksum: "test",
        bytes: 10,
        quality: "VALIDATED",
        coverage: "RETURNED_ROWS_ONLY",
        version_semantics: "CUMULATIVE",
        partitions: [],
        transform: null,
      },
    };
    const updated = { ...old, id: "daily-new", rows: 26 };
    const gap = {
      id: "gap-fixed",
      checker: "daily-coverage-v2",
          identity: coverageIdentity(), reference_symbol: null,
      created_at: 1,
      as_of: "2026-09-19",
      daily_version_id: old.id,
      calendar_version_id: "calendar-fixed",
      contracts_version_id: "contracts-fixed",
      source: "tushare",
      exchange: "SHFE",
      symbol: "RB2405.SHF",
      start: "2024-01-01",
      end: "2024-01-31",
      status: "GAPS",
      counts: { GAP: 1, PRESENT: 25 },
      days: [
        {
          date: "2024-01-15",
          status: "GAP",
          has_data: false,
          reason: "缺少记录",
        },
      ],
      refill_ranges: [{ start: "2024-01-15", end: "2024-01-15" }],
      notes: [],
    };
    const covered = {
      ...gap,
      id: "covered-new",
      daily_version_id: updated.id,
      status: "COVERED",
      counts: { PRESENT: 26 },
      days: [],
      refill_ranges: [],
    };
    const failed = {
      id: "fill-failed",
      command_id: "fill",
      kind: "data.sync",
      state: "FAILED",
      attempt: 1,
      created_at: 2,
      error: "提供方暂不可用",
      result: null,
    };
    const success = {
      ...failed,
      id: "fill-retry",
      state: "SUCCEEDED",
      error: null,
      result: { version_id: updated.id, completed: 1, total: 1 },
    };
    const commands: string[] = [];
    let exactReport = false,
      submitted = false;
    let research: any = null;
    let draft: any = null;
    let tracked: any[] = [];
    let readFailure = false;
    await context.route("**/api/v1/**", (route) => {
      if (new URL(route.request().url()).pathname.endsWith("/research/experiments")) return route.fulfill({ json: { items: [] } });
      if (
        new URL(route.request().url()).pathname.endsWith("/research/strategies")
      )
        return route.fulfill({ json: strategies });
      if (route.request().url().endsWith("/contract-rules"))
        return rulesRoute(route);
      if (route.request().url().endsWith("/access/scopes"))
        return route.fulfill({
          json: { token: "scope-fixture", expires: Date.now() / 1000 + 300 },
        });

      const r = route.request(),
        p = new URL(r.url()).pathname;
      if (p.includes("/security")) return route.fallback();
      if (p.endsWith("/research/workspace"))
        return route.fulfill({ json: { draft, templates: [] } });
      if (p.endsWith("/research/workspace/draft")) {
        draft = {
          id: "draft",
          ...r.postDataJSON(),
          revision: r.postDataJSON().expected_revision + 1,
        };
        return route.fulfill({ json: draft });
      }
      if (p.endsWith("/refill-status") && readFailure)
        return route.fulfill({
          status: 503,
          json: { detail: "状态服务暂不可用" },
        });
      if (p.endsWith("/refill-status"))
        return route.fulfill({
          json: {
            report_id: p.split("/").at(-2),
            submitted: tracked.length > 0,
            statuses: tracked.length ? ["QUEUED"] : [],
            jobs: tracked,
            total: tracked.length,
            truncated: false,
          },
        });
      if (p.endsWith("/data/types")) return route.fulfill({ json: [type] });
      if (p.includes("/data/catalog"))
        return route.fulfill({ json: { items: [old], total: 1, offset: 0 } });
      if (p.endsWith("/data/coverage/gap-fixed")) {
        exactReport = true;
        return route.fulfill({ json: gap });
      }
      if (p.endsWith("/refill")) {
        commands.push(r.postDataJSON().command_id);
        if (commands.length === 1) {
          if (lostResponse) tracked = [failed];
          return route.fulfill({
            status: 503,
            json: { detail: "连接中断，请重试" },
          });
        }
        tracked = [failed];
        return route.fulfill({
          json: {
            status: "QUEUED",
            report_id: gap.id,
            checked_report_id: gap.id,
            jobs: [{ ...failed, state: "QUEUED", error: null }],
            skipped_gap_days: 0,
          },
        });
      }
      if (p.endsWith("/retry")) {
        expect(r.postDataJSON().resume).toBe(true);
        tracked = [success];
        return route.fulfill({
          json: { ...success, state: "QUEUED", result: null },
        });
      }
      if (p.endsWith("/jobs/fill-failed"))
        return route.fulfill({ json: failed });
      if (p.endsWith("/jobs/fill-retry"))
        return route.fulfill({ json: success });
      if (p.endsWith("/coverage")) {
        const body = r.postDataJSON();
        if (body?.use_latest_daily || p.includes(updated.id)) {
          expect(body.calendar_version_id).toBe(gap.calendar_version_id);
          expect(body.contracts_version_id).toBe(gap.contracts_version_id);
          return route.fulfill({ json: covered });
        }
        return route.fulfill({ json: gap });
      }
      if (p.includes("/data/versions/"))
        return route.fulfill({
          json: {
            version: p.includes(updated.id) ? updated : old,
            rows: [],
            total: 25,
            offset: 0,
            snapshot: null,
          },
        });
      if (p.endsWith("/research/runs") && r.method() === "POST") {
        const body = r.postDataJSON();
        expect(body.version_id).toBe(updated.id);
        expect(body.coverage_report_id).toBe(covered.id);
        expect(body.start).toBe(gap.start);
        expect(body.end).toBe(gap.end);
        expect(body.rules.spec.multiplier).toBe("10");
        submitted = true;
        research = {
          id: "run-new",
          kind: "research.backtest",
          state: "QUEUED",
          attempt: 0,
          created_at: 4,
          request: body,
          version: updated,
          coverage: covered,
          contract: "SHFE.rb2405",
          engine: "daily-rules-next-open.v2",
          input_checksum: "frozen",
          warnings: [],
          error: null,
          output: null,
          result: null,
        };
        return route.fulfill({ json: research, status: 202 });
      }
      if (p.endsWith("/research/runs"))
        return route.fulfill({ json: research ? [research] : [] });
      if (p.includes("/research/runs/"))
        return route.fulfill({ json: research });
      const user = {
        email: "refill@example.com",
        first_name: "Refill",
        last_name: "Test",
      };
      return route.fulfill({
        json: p.endsWith("/login")
          ? { session: "session", user }
          : p.endsWith("/me")
            ? user
            : p.endsWith("/health")
              ? { status: "ready" }
              : [],
      });
    });
    await page.goto("/");
    await page.getByLabel("邮箱", { exact: true }).fill("refill@example.com");
    await page.getByLabel("密码", { exact: true }).fill("test-refill-password");
    await page.getByRole("button", { name: "登录", exact: true }).click();
    await page.getByRole("button", { name: "研究", exact: true }).click();
    await page
      .getByRole("combobox", { name: "策略", exact: true })
      .selectOption({ label: "双均线 · 1.0.0" });
    await page
      .getByRole("combobox", { name: "日线数据集", exact: true })
      .selectOption(old.dataset_id);
    await selectRules(page);
    await page
      .getByRole("button", { name: "核对所选版本覆盖", exact: true })
      .click();
    await page
      .getByRole("button", { name: "查看对应数据与报告", exact: true })
      .click();
    await expect.poll(() => exactReport).toBe(true);
    await page
      .getByRole("button", { name: "返回研究（保留参数）", exact: true })
      .click();
    await expect(page.getByLabel("固定合约规则")).toContainText("乘数 10");
    const panel = page.getByRole("region", {
      name: "研究缺口处理",
      exact: true,
    });
    await panel
      .getByRole("button", { name: "补齐确认缺口", exact: true })
      .click();
    await expect(panel.getByRole("alert")).toContainText("连接中断");
    if (!lostResponse) {
      await panel
        .getByRole("button", { name: "补齐确认缺口", exact: true })
        .click();
      expect(commands[0]).toBe(commands[1]);
    }
    await expect(
      panel.getByText("提供方暂不可用", { exact: true }),
    ).toBeVisible();
    await expect
      .poll(() => draft?.content.config.coverage_report_id)
      .toBe(gap.id);
    const submissions = commands.length;
    await page.reload();
    await page.getByRole("button", { name: "研究", exact: true }).click();
    await expect(
      panel.getByText("提供方暂不可用", { exact: true }),
    ).toBeVisible();
    await expect(
      panel.getByRole("button", { name: "补齐确认缺口", exact: true }),
    ).toBeDisabled();
    expect(commands.length).toBe(submissions);
    if (lostResponse) {
      readFailure = true;
      await expect(panel.getByRole("alert")).toContainText("补齐记录读取失败");
      await expect(
        panel.getByRole("button", { name: "继续补齐", exact: true }),
      ).toBeDisabled();
      readFailure = false;
      await expect(
        panel.getByRole("button", { name: "继续补齐", exact: true }),
      ).toBeEnabled();
    }
    await panel.getByRole("button", { name: "继续补齐", exact: true }).click();
    await expect(panel.getByText("已发布", { exact: true })).toBeVisible();
    await page.reload();
    await page.getByRole("button", { name: "研究", exact: true }).click();
    await expect(panel.getByText("已发布", { exact: true })).toBeVisible();
    await expect(
      panel.getByRole("button", { name: "继续补齐", exact: true }),
    ).toHaveCount(0);
    expect(commands.length).toBe(submissions);
    await expect(
      page.getByRole("combobox", { name: "固定数据版本", exact: true }),
    ).toHaveValue(old.id);
    await panel
      .getByRole("button", { name: "检查补齐后的版本", exact: true })
      .click();
    await expect(
      page.getByRole("combobox", { name: "固定数据版本", exact: true }),
    ).toHaveValue(old.id);
    await panel
      .getByRole("button", { name: "采用此版本并重新核对", exact: true })
      .click();
    await expect(
      page.getByRole("combobox", { name: "固定数据版本", exact: true }),
    ).toHaveValue(updated.id);
    await expect(
      page.getByText("所选依据下记录齐全", { exact: true }),
    ).toBeVisible();
    await page.getByRole("checkbox", { name: /我接受历史收盘/ }).check();
    await page.getByRole("button", { name: "运行回测", exact: true }).click();
    await expect.poll(() => submitted).toBe(true);
  });
}
