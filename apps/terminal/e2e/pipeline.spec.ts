import { expect, test } from "@playwright/test";
test("real worker publishes data and UI restores the task after reload", async ({
  page,
}) => {
  await page.goto("/");
  await page.getByLabel("会话令牌").fill(process.env.ASTERION_TOKEN!);
  await page.getByRole("button", { name: "连接", exact: true }).click();
  await expect(page.getByText("● 后台已连接")).toBeVisible();
  await page
    .getByRole("navigation", { name: "业务工作区" })
    .getByRole("button", { name: "数据", exact: true })
    .click();
  await page.getByLabel("数据源", { exact: true }).selectOption("local_file");
  await page
    .getByPlaceholder("供应商 / 文件来源 / 数据频率")
    .fill("E2E synthetic fixture — not market data");
  await page
    .getByLabel("CSV 数据")
    .fill(
      "contract,event_time,available_at,trading_day,open,high,low,close,volume\nSHFE.rb2610,2026-09-14T13:00:00Z,2026-09-14T13:01:00Z,2026-09-15,3200,3220,3190,3210,100\nSHFE.rb2610,2026-09-14T13:01:00Z,2026-09-14T13:02:00Z,2026-09-15,3210,3230,3200,3220,120",
    );
  const submitted = page.waitForResponse(
    (r) =>
      r.url().endsWith("/api/v1/imports") && r.request().method() === "POST",
  );
  await page.getByRole("button", { name: "创建采集任务" }).click();
  const job = await (await submitted).json();
  const jobRow = page.getByRole("row").filter({ hasText: job.id.slice(0, 8) });
  await expect(page.getByRole("status")).toContainText("请求已接收");
  await expect(jobRow.getByText("已完成", { exact: true })).toBeVisible({
    timeout: 20000,
  });
  const response = await page.request.get(
    "http://127.0.0.1:8000/api/v1/snapshots",
    {
      headers: { Authorization: `Bearer ${process.env.ASTERION_TOKEN}` },
    },
  );
  const snapshot = (await response.json()).find(
    (s: { job_id: string }) => s.job_id === job.id,
  );
  expect(snapshot).toBeTruthy();
  await page.getByRole("button", { name: "数据集", exact: true }).click();
  await page
    .getByRole("button", { name: "历史行情 / 文件", exact: true })
    .first()
    .click();
  await page.getByRole("button", { name: "查看历史图表", exact: true }).click();
  await expect(page.locator("canvas").first()).toBeVisible();
  await page.getByRole("button", { name: "检查器", exact: true }).click();
  await expect(page.getByText("PUBLISHED", { exact: true })).toBeVisible();
  await page.screenshot({
    path: "../../.state/terminal-market.png",
    fullPage: true,
  });
  await page.reload();
  await expect(
    page.getByRole("heading", { name: "市场", exact: true }),
  ).toBeVisible();
  await page.getByLabel("会话令牌").fill(process.env.ASTERION_TOKEN!);
  await page.getByRole("button", { name: "连接", exact: true }).click();
  await expect(jobRow.getByText("已完成", { exact: true })).toBeVisible();
});

test("invalid token can be corrected without reloading", async ({ page }) => {
  await page.goto("/");
  await page.getByLabel("会话令牌").fill("invalid-token");
  await page.getByRole("button", { name: "连接", exact: true }).click();
  await expect(page.getByRole("alert")).toContainText("Invalid session token");
  await page.getByLabel("会话令牌").fill(process.env.ASTERION_TOKEN!);
  await page.getByRole("button", { name: "连接", exact: true }).click();
  await expect(page.getByText("● 后台已连接")).toBeVisible();
});
