import { expect, test } from "@playwright/test";
import { nativeContext } from "./native";

test("role diagnostics distinguishes scope, publication evidence and failed refresh", async ({ context, page }) => {
  await nativeContext(context);
  let fail = false;
  let retries = 0;
  const batches: unknown[] = [];
  let registered = false;
  let complete = false;
  const workflow = { id: "workflow-case", request: { trading_day: "2025-04-14", previous_version_id: "computed-fixture", sync_job_ids: ["daily-a", "daily-b"] }, job_id: null, error: null };

  const failedJob = { id: "role-failed", command_id: "role-case", kind: "contract_roles.continue", state: "FAILED", attempt: 1, created_at: 1, error: "offline failure", result: null };
  const user = { email: "roles@example.com", first_name: "Role", last_name: "Test" };
  await context.route("**/api/v1/**", (route) => {
    const path = new URL(route.request().url()).pathname;
    if (path.endsWith("/computed/tasks/role-failed/retry")) {
      retries++;
      return route.fulfill({ json: { ...failedJob, id: "role-retry", state: "QUEUED", error: null } });
    }
    if (path.endsWith("/sync-plan")) return route.fulfill({ json: { previous_version_id: "computed-fixture", trading_day: "2025-04-14", provider: "offline", symbols: ["AU2506", "AU2508"] } });
    if (path.endsWith("/computed/computed-fixture/sync-workflows")) return route.fulfill({ json: registered ? [workflow] : [] });
    if (path.endsWith("/data/providers")) return route.fulfill({ json: [{ id: "connection-a", plugin_id: "offline", connection_id: "connection-a", name: "研究连接", configured: true, lifecycle: { state: "enabled" } }, { id: "disabled", plugin_id: "offline", name: "停用连接", configured: true, lifecycle: { state: "disabled" } }] });
    if (path.endsWith("/sync-batches")) {
      batches.push(route.request().postDataJSON());
      if (batches.length === 1) return route.abort("failed");
      registered = true;
      return route.fulfill({ json: workflow });
    }
    if (path.endsWith("/sync-workflows/workflow-case")) return route.fulfill({ json: { ...workflow, job_id: complete ? "role-done" : null, dependencies: [{ id: "daily-a", symbol: "AU2506", state: "SUCCEEDED", version_id: "fixed-a" }, { id: "daily-b", symbol: "AU2508", state: complete ? "SUCCEEDED" : "RUNNING", version_id: complete ? "fixed-b" : null }] } });
    if (path.endsWith("/computed/tasks/role-done")) return route.fulfill({ json: { ...failedJob, id: "role-done", state: "SUCCEEDED", error: null, result: { computed_version_id: "published-next-version" } } });
    if (path.endsWith("/jobs")) return route.fulfill({ json: [failedJob] });
    if (path.includes("/account/security")) return route.fallback();
    if (path.endsWith("/access/scopes")) return route.fulfill({ json: { token: "scope-fixture", expires: Date.now() / 1000 + 300 } });
    if (path.endsWith("/data/catalog")) return route.fulfill({ json: { items: [], total: 0 } });
    if (path.endsWith("/contract-roles/computed")) {
      if (fail) return route.fulfill({ status: 422, json: { detail: "固定来源证据不可用" } });
      return route.fulfill({ json: [{ id: "computed-fixture", published_at: "2025-04-11T16:00:00+08:00", spec: {
        origin: "computed", request: { product_id: "SHFE.AU", contracts_version_id: "fixed-contracts", daily_inputs: [], policy: { metric: "open_interest", switch_margin: "0.1", confirmations: 2 } },
        candidates: { coverage: "explicit_subset", included: ["AU2506", "AU2508"], excluded: [{ symbol: "AU2502", contract_id: "expired-au", reason: "expired_before_window" }] },
        result: { input_digest: "digest", decisions: [{ observation_day: "2025-04-11", available_at: "2025-04-11T15:05:00+08:00", effective_day: "2025-04-14", effective_start: "2025-04-11T21:00:00+08:00", main: "AU2508", secondary: "AU2506", reason: "switched", confirmation_count: 2, excluded: [] }] },
        artifact: { algorithm: "daily-role-ranking.v1", checksum: "fixture-checksum" },
      } }] });
    }
    if (path.endsWith("/contract-roles")) return route.fulfill({ json: [{ id: "provider-fixture", spec: { origin: "provider_report", product_id: "SHFE.AU", source: "offline-fixture", source_version: "fixed-report", reports: [{ trading_day: "2025-04-14", role: "main", contract_id: "AU2508", available_at: null }] } }] });
    return route.fulfill({ json: path.endsWith("/login") ? { session: "session", user } : path.endsWith("/me") ? user : path.endsWith("/health") ? { status: "ready" } : [] });
  });
  await page.goto("/");
  await page.getByLabel("邮箱", { exact: true }).fill(user.email);
  await page.getByLabel("密码", { exact: true }).fill("password-test-123");
  await page.getByRole("button", { name: "登录", exact: true }).click();
  await page.getByRole("navigation", { name: "业务工作区" }).getByRole("button", { name: "数据", exact: true }).click();
  await page.getByRole("button", { name: "合约角色", exact: true }).click();
  const panel = page.getByRole("region", { name: "合约角色诊断" });
  await expect(panel.getByText("手动指定子集", { exact: false })).toBeVisible();
  await expect(panel.getByText("2025-04-11T21:00:00+08:00", { exact: false })).toBeVisible();
  await expect(panel.getByText("切换 · 2")).toBeVisible();
  await panel.getByText("候选与排除依据", { exact: true }).click();
  await expect(panel.getByText("观测窗口前已到期", { exact: false })).toBeVisible();
  await panel.getByRole("button", { name: "采集与续算", exact: true }).click();
  const batch = panel.getByRole("region", { name: "采集与续算" });
  await expect(batch.getByText("观测交易日：2025-04-14", { exact: false })).toBeVisible();
  await expect(batch.getByLabel("采集连接").getByRole("option")).toHaveCount(1);
  await batch.getByRole("button", { name: "采集并续算", exact: true }).click();
  await expect(batch.getByRole("alert")).toContainText("可重试原请求");
  await expect(batch.getByLabel("采集连接")).toBeDisabled();
  await batch.getByRole("button", { name: "重试原请求", exact: true }).click();
  expect(batches).toHaveLength(2);
  expect(batches[1]).toEqual(batches[0]);
  await expect(batch.getByRole("cell", { name: "执行中", exact: true })).toBeVisible();
  complete = true;
  await batch.getByRole("button", { name: "刷新采集进度", exact: true }).click();
  await expect(batch.getByText("published-next-version", { exact: true })).toBeVisible();
  await panel.getByRole("button", { name: "供应商报告", exact: true }).click();
  await panel.getByRole("button", { name: "计算角色", exact: true }).click();
  await panel.getByRole("button", { name: "采集与续算", exact: true }).click();
  await expect(batch.getByText("published-next-version", { exact: true })).toBeVisible();
  expect(batches).toHaveLength(2);
  await page.screenshot({ path: "../../.state/f2-roles-panel.png", fullPage: true });
  await panel.getByRole("button", { name: "供应商报告", exact: true }).click();
  await expect(panel.getByRole("cell", { name: "未知", exact: true })).toBeVisible();
  await expect(panel.getByText("手动指定子集", { exact: false })).toHaveCount(0);
  await page.getByRole("button", { name: "任务", exact: true }).click();
  const dock = page.getByRole("region", { name: "任务中心" });
  await expect(dock.getByText("合约角色续算", { exact: true })).toBeVisible();
  await dock.getByRole("button", { name: "重试续算", exact: true }).click();
  await expect(dock.getByText("已接收", { exact: true })).toBeVisible();
  expect(retries).toBe(1);
  fail = true;
  await panel.getByRole("button", { name: "计算角色", exact: true }).click();
  await expect(panel.getByRole("alert")).toContainText("固定来源证据不可用");
  await expect(panel.getByText("computed-fixture", { exact: true })).toHaveCount(0);
});
