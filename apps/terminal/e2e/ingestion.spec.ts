import { expect, test } from "@playwright/test";
import { nativeContext } from "./native";

test("task evidence distinguishes received data, failures and resumed inputs", async ({
  context,
  page,
}) => {
  await nativeContext(context);
  let corrupt = false;
  let retryBody: unknown;
  const job = {
    id: "evidence-job",
    command_id: "fixture",
    kind: "data.sync",
    state: "FAILED",
    attempt: 2,
    created_at: 1789600000,
    error: "分段采集失败",
    result: null,
  };
  const records = Array.from({ length: 21 }, (_, index) => ({
    job_id: job.id,
    attempt: index === 20 ? 2 : 1,
    partition_index: index === 20 ? 0 : index,
    manifest: {
      checksum: "synthetic-checksum",
      uri: "asterion://local/synthetic",
      bytes: 1024,
      rows: index === 0 ? 51 : 0,
      observed_at: "2024-01-03T00:00:00Z",
      plugin_version: "1.0.0",
      status:
        index === 0 || index === 20
          ? "RECEIVED"
          : index === 1
            ? "PERMISSION_DENIED"
            : "EMPTY_UNCONFIRMED",
      partition: {
        api: "fut_daily",
        params: {
          exchange: "SHFE",
          ts_code: "RB2610.SHF",
          start_date: "20240102",
          end_date: "20240201",
        },
        fields: ["trade_date", "close"],
        limit: 2000,
      },
      reused_from:
        index === 20
          ? {
              job_id: "prior-job",
              attempt: 1,
              partition_index: 0,
              checksum: "source-checksum",
            }
          : null,
    },
  }));
  await context.route("**/api/v1/**", (route) => {
    if (route.request().url().endsWith("/access/scopes")) return route.fulfill({ json: { token: "scope-fixture", expires: Date.now() / 1000 + 300 } });

    const url = new URL(route.request().url()),
      path = url.pathname;
    if (path.includes("/account/security")) return route.fallback();
    if (path.endsWith("/retry")) {
      retryBody = route.request().postDataJSON();
      return route.fulfill({
        status: 202,
        json: { ...job, id: "retry-job", state: "QUEUED", error: null },
      });
    }
    if (path.endsWith("/observations")) {
      const offset = Number(url.searchParams.get("offset"));
      return route.fulfill({
        json: { items: records.slice(offset, offset + 20), total: 21 },
      });
    }
    if (path.includes("/observations/")) {
      if (corrupt)
        return route.fulfill({
          status: 422,
          json: { detail: "原始证据校验和不一致" },
        });
      const index = Number(path.split("/").at(-1));
      const offset = Number(url.searchParams.get("offset"));
      const rows =
        index === 0
          ? Array.from({ length: 51 }, (_, i) => ({
              trade_date: "20240102",
              close: `price-${i}`,
            }))
          : [];
      return route.fulfill({
        json: {
          manifest: records[index].manifest,
          rows: rows.slice(offset, offset + 50),
          total: rows.length,
        },
      });
    }
    return route.fulfill({
      json: path.endsWith("/jobs")
        ? [job]
        : path.endsWith("/login")
          ? {
              session: "session",
              user: {
                email: "evidence@example.com",
                first_name: "Evidence",
                last_name: "Test",
              },
            }
          : path.endsWith("/me")
            ? {
                email: "evidence@example.com",
                first_name: "Evidence",
                last_name: "Test",
              }
            : path.endsWith("/health")
              ? { status: "ready" }
              : [],
    });
  });
  await page.goto("/");
  await page.getByLabel("邮箱", { exact: true }).fill("evidence@example.com");
  await page
    .getByLabel("密码", { exact: true })
    .fill("synthetic-test-password");
  await page.getByRole("button", { name: "登录", exact: true }).click();
  await page.getByRole("button", { name: "任务", exact: true }).click();
  await page.getByRole("button", { name: "采集详情", exact: true }).click();
  const detail = page.getByRole("region", { name: "任务中心" });
  await expect(
    detail.getByText("已接收仅表示", { exact: false }),
  ).toBeVisible();
  await expect(
    detail.getByRole("cell", { name: "接口权限不足", exact: true }),
  ).toBeVisible();
  await detail.getByRole("button", { name: "查看原始数据" }).first().click();
  await expect(
    detail.getByRole("cell", { name: "price-0", exact: true }),
  ).toBeVisible();
  await detail.getByRole("button", { name: "下一页原始数据" }).click();
  await expect(
    detail.getByRole("cell", { name: "price-50", exact: true }),
  ).toBeVisible();
  await expect(
    detail.getByRole("button", { name: "下一页原始数据" }),
  ).toBeDisabled();
  corrupt = true;
  await detail.getByRole("button", { name: "刷新采集详情" }).click();
  await expect(detail.getByRole("alert")).toContainText("校验和不一致");
  await expect(
    detail.getByRole("cell", { name: "price-50", exact: true }),
  ).toHaveCount(0);
  corrupt = false;
  await detail.getByRole("button", { name: "返回分段列表" }).click();
  await detail.getByRole("button", { name: "查看原始数据" }).nth(1).click();
  await expect(
    detail.getByText("此分段没有原始数据行", { exact: false }),
  ).toBeVisible();
  await detail.getByRole("button", { name: "返回分段列表" }).click();
  await detail.getByRole("button", { name: "下一页分段" }).click();
  await expect(
    detail.getByRole("cell", { name: "已接收 · 待发布校验 · 已复用" }),
  ).toBeVisible();
  await page.screenshot({ path: "../../.state/ingestion-details.png" });
  await detail.getByRole("button", { name: "返回任务列表" }).click();
  await detail.getByRole("button", { name: "继续同步", exact: true }).click();
  await expect.poll(() => retryBody).toMatchObject({ resume: true });
  await detail
    .getByRole("button", { name: "全部重新同步", exact: true })
    .click();
  await expect.poll(() => retryBody).toMatchObject({ resume: false });
});
