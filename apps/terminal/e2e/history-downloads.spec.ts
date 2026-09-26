import { expect, test } from "@playwright/test";
import { nativeContext } from "./native";
import { providerFixture } from "./provider-fixture";

test("history selection, response loss, coverage and saved batch recovery", async ({
  context,
  page,
}) => {
  await nativeContext(context);
  const user = {
    email: "history@example.com",
    first_name: "History",
    last_name: "Test",
  };
  const provider = providerFixture({
    id: "tushare",
    name: "Tushare",
    version: "1",
    api_version: 2,
    configured: true,
    demo: false,
    capabilities: [
      {
        id: "daily",
        type_id: "futures.daily",
        label: "历史日线",
        exchanges: ["SHFE"],
        date_range: true,
        symbol_required: true,
        description: "日线",
      },
    ],
  });
  const ref = (type: string) => ({
    id: `${type}-fixed`,
    dataset_id: `${type}-set`,
    created_at: 1789600000,
    manifest: {
      scope: { exchange: "SHFE" },
      first: "2024-01-01",
      last: "2024-02-29",
    },
  });
  let referenceFailure = true,
    responseLoss = true,
    coverageFailure = true;
  const previews: any[] = [],
    submissions: any[] = [];
  let batch: any = null;
  await context.route("**/api/v1/**", async (route) => {
    const url = new URL(route.request().url()),
      path = url.pathname;
    if (path.includes("/account/security")) return route.fallback();
    if (path.endsWith("/access/scopes"))
      return route.fulfill({
        json: { token: "history-scope", expires: Date.now() / 1000 + 300 },
      });
    if (path.endsWith("/data/providers"))
      return route.fulfill({ json: [provider] });
    if (path.endsWith("/data/catalog")) {
      if (referenceFailure)
        return route.fulfill({ status: 503, json: { detail: "资料暂不可用" } });
      const type =
        url.searchParams.get("type_id") === "futures.calendar"
          ? "calendar"
          : "contracts";
      return route.fulfill({ json: { items: [ref(type)], total: 1 } });
    }
    if (path.includes("/data/catalog/") && path.endsWith("/versions"))
      return route.fulfill({
        json: {
          items: [ref(path.includes("calendar") ? "calendar" : "contracts")],
          total: 1,
        },
      });
    if (path.endsWith("/data/versions/contracts-fixed"))
      return route.fulfill({
        json: {
          total: 2,
          rows: ["RB", "HC"].map((product) => ({
            symbol: `${product}2405.SHF`,
            product,
            delivery_month: "2024-05",
            listed: "2024-01-02",
            delisted: "2024-05-15",
          })),
        },
      });
    if (path.endsWith("/data/history/plan")) {
      const body = route.request().postDataJSON();
      previews.push(body);
      return route.fulfill({
        json: {
          request: body,
          expected_days: body.symbols.length * 20,
          slices: body.symbols.map((symbol: string, i: number) => ({
            contract_id: `SHFE.${symbol.slice(0, 2)}.202405.20240102`,
            expected_days: 20,
            request: { ...body, symbol, command_id: `part-${i}` },
          })),
        },
      });
    }
    if (path.endsWith("/data/history")) {
      if (route.request().method() === "GET")
        return route.fulfill({
          json: batch
            ? [
                {
                  request: batch.plan.request,
                  created_at: 1789600000,
                  task_count: batch.tasks.length,
                },
              ]
            : [],
        });
      const body = route.request().postDataJSON();
      submissions.push(body);
      batch = {
        plan: { request: body, slices: [], expected_days: 40 },
        tasks: body.symbols.map((symbol: string, i: number) => ({
          id: `history-job-${i}`,
          command_id: `history-${i}`,
          kind: "data.sync",
          state: "SUCCEEDED",
          created_at: 1789600000,
          attempt: 1,
          error: null,
          result: {},
        })),
      };
      if (responseLoss) {
        responseLoss = false;
        return route.fulfill({
          status: 503,
          json: { detail: "响应丢失，请重试" },
        });
      }
      return route.fulfill({ status: 202, json: batch });
    }
    if (path.includes("/data/history/") && path.endsWith("/coverage")) {
      if (coverageFailure) {
        coverageFailure = false;
        return route.fulfill({
          status: 503,
          json: { detail: "覆盖核对暂不可用" },
        });
      }
      return route.fulfill({
        json: {
          command_id: batch.plan.request.command_id,
          complete: false,
          counts: { PRESENT: 39, GAP: 1 },
          items: batch.plan.request.symbols.map(
            (symbol: string, i: number) => ({
              symbol,
              contract_id: `SHFE.${symbol.slice(0, 2)}.202405.20240102`,
              error: null,
              report: {
                id: `report-${i}`,
                daily_version_id: `daily-${i}`,
                status: i ? "GAPS" : "COVERED",
                counts: { GAP: i },
              },
            }),
          ),
        },
      });
    }
    if (path.includes("/data/history/")) return route.fulfill({ json: batch });
    if (path.endsWith("/jobs"))
      return route.fulfill({ json: batch?.tasks ?? [] });
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
  await page.getByLabel("邮箱", { exact: true }).fill(user.email);
  await page
    .getByLabel("密码", { exact: true })
    .fill("synthetic-history-password");
  await page.getByRole("button", { name: "登录", exact: true }).click();
  await page
    .getByRole("navigation", { name: "业务工作区" })
    .getByRole("button", { name: "数据", exact: true })
    .click();
  await page.getByRole("button", { name: "批量下载", exact: true }).click();
  const panel = page.getByRole("region", { name: "批量历史下载" });
  await expect(panel.getByRole("alert")).toContainText("资料暂不可用");
  referenceFailure = false;
  await panel.getByRole("button", { name: "刷新资料", exact: true }).click();
  await panel.getByLabel("批量开始日期").fill("2024-01-01");
  await panel.getByLabel("批量结束日期").fill("2024-01-31");
  await panel.getByLabel("批量合约资料").selectOption("contracts-fixed");
  await panel.getByLabel("批量交易日历").selectOption("calendar-fixed");
  await panel.getByLabel("搜索批量合约").fill("RB");
  await panel
    .getByRole("button", { name: "选择当前显示", exact: true })
    .click();
  await panel
    .getByRole("button", { name: "核对下载范围", exact: true })
    .click();
  await expect(panel.getByText(/将提交 1 个/)).toBeVisible();
  await panel.getByRole("button", { name: "修改范围", exact: true }).click();
  await panel.getByLabel("搜索批量合约").fill("");
  await panel
    .getByRole("button", { name: "选择当前显示", exact: true })
    .click();
  await page.screenshot({
    path: "../../.state/history-selection.png",
    fullPage: true,
  });
  await panel
    .getByRole("button", { name: "核对下载范围", exact: true })
    .click();
  await expect(panel.getByText(/将提交 2 个/)).toBeVisible();
  await panel.getByRole("button", { name: "确认下载", exact: true }).click();
  await expect(panel.getByRole("alert")).toContainText("响应丢失");
  await expect(
    panel.getByRole("button", { name: "修改范围", exact: true }),
  ).toBeDisabled();
  await panel
    .getByRole("button", { name: "重试确认下载", exact: true })
    .click();
  await expect(panel.getByText(/下载任务：成功 2/)).toBeVisible();
  expect(submissions).toHaveLength(2);
  expect(submissions[0]).toEqual(submissions[1]);
  expect(previews[1].symbols).toEqual(["HC2405.SHF", "RB2405.SHF"]);
  await panel.getByRole("button", { name: "核对覆盖", exact: true }).click();
  await expect(panel.getByRole("alert")).toContainText("覆盖核对暂不可用");
  await panel.getByRole("button", { name: "核对覆盖", exact: true }).click();
  await expect(panel.getByRole("status")).toContainText("缺口 1");
  await page.screenshot({
    path: "../../.state/history-coverage.png",
    fullPage: true,
  });
  await page.getByRole("button", { name: "单合约同步", exact: true }).click();
  await page.getByRole("button", { name: "批量下载", exact: true }).click();
  await panel
    .getByLabel("最近下载")
    .selectOption(batch.plan.request.command_id);
  await expect(panel.getByText(/下载任务：成功 2/)).toBeVisible();
  await expect(panel.getByRole("status")).toHaveCount(0);
  await page.reload();
  await page
    .getByRole("navigation", { name: "业务工作区" })
    .getByRole("button", { name: "数据", exact: true })
    .click();
  await page.getByRole("button", { name: "批量下载", exact: true }).click();
  await panel
    .getByLabel("最近下载")
    .selectOption(batch.plan.request.command_id);
  await expect(panel.getByText(/下载任务：成功 2/)).toBeVisible();
  await panel.getByRole("button", { name: "新建下载", exact: true }).click();
  await panel
    .getByRole("button", { name: "返回下载结果", exact: true })
    .click();
  await expect(panel.getByText(/下载任务：成功 2/)).toBeVisible();
});
