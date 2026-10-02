import { test, expect } from "./test";
import { seedHistory, rpc } from "./dataset-fixture";

test("a downloaded dataset reaches a fixed backtest result without reselecting its source", async ({
  page,
}) => {
  await page.goto("/");
  const selected = await seedHistory(
    page.request,
    [100, 101, 102, 101, 104, 103, 102, 103],
    "flow-source",
  );
  await page.reload();
  await page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }).click();
  await page.getByLabel("数据源", { exact: true }).selectOption("tushare.ft_mins");
  await page
    .locator(`[data-task-id="flow-source-bars"]`)
    .getByRole("button", { name: "用于回测" })
    .click();
  await expect(page.getByRole("heading", { name: "新建回测", exact: true })).toBeVisible();
  await expect(page.getByLabel("K 线来源", { exact: true })).toHaveValue(
    selected.source_dataset_ids[0],
  );
  if (await page.getByLabel("结算价来源", { exact: true }).isVisible())
    await page
      .getByLabel("结算价来源", { exact: true })
      .selectOption(selected.settlement_dataset_ids[0]);
  await expect(page.getByLabel("结算价来源", { exact: true })).toHaveValue(
    selected.settlement_dataset_ids[0],
  );
  await page.getByLabel("最小变动价位", { exact: true }).fill(selected.price_increment);
  await page.getByLabel("合约乘数", { exact: true }).fill(selected.multiplier);
  await page.getByLabel("开始交易日", { exact: true }).fill("2026-09-25");
  await page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }).click();
  await page.locator(".workspace-tabs").getByRole("button", { name: "研究", exact: true }).click();
  await expect(page.getByLabel("K 线来源", { exact: true })).toHaveValue(
    selected.source_dataset_ids[0],
  );
  await expect(page.getByLabel("开始交易日", { exact: true })).toHaveValue("2026-09-25");
  await page.getByRole("button", { name: "使用此数据集", exact: true }).click();
  await expect(page.getByRole("list", { name: "已选合约" })).toContainText("8 根 · 1 个交易日");
  await page.getByRole("button", { name: "下一步", exact: true }).click();
  for (const [label, value] of [
    ["快均线", "1"],
    ["慢均线", "3"],
    ["目标手数", "1"],
    ["初始资金", "10000"],
    ["每手保证金", "100"],
    ["每手开仓费", "2"],
    ["每手平今费", "3"],
    ["每手平昨费", "4"],
    ["单笔数量上限", "100"],
    ["总持仓量上限", "100"],
    ["在途委托数上限", "100"],
  ])
    await page.getByLabel(label, { exact: true }).fill(value);
  await page.getByRole("button", { name: "下一步", exact: true }).click();
  await page.getByRole("button", { name: "开始回测", exact: true }).click();
  const progress = page.getByRole("region", { name: "回测进度", exact: true });
  await progress.getByRole("button", { name: "查看结果" }).click({ timeout: 20000 });
  const result = page.getByRole("region", { name: "回测结果", exact: true });
  await expect(result.getByRole("img", { name: "权益曲线" })).toBeVisible();
  await result.getByText("实验参数", { exact: true }).click();
  await expect(result).toContainText(selected.source_dataset_ids[0]);
  await page.screenshot({
    path: "apps/clients/terminal/test-results/download-backtest-result.png",
  });
});

test("data handoff stays bound to its service and never reuses another contract's units", async ({
  page,
}) => {
  await page.goto("/");
  await seedHistory(page.request, [100, 101, 102], "handoff-rb");
  const two = await seedHistory(page.request, [100, 101, 102], "handoff-hc", {
    product: "hc",
    keep: true,
  });
  await rpc(page.request, "research.dataset.select", two);
  let identity = "handoff-service";
  await page.route("**/__asterion/api", async route => {
    if (route.request().postDataJSON().method !== "runtime.snapshot") return route.continue();
    const response = await route.fetch();
    const body = await response.json();
    if (body.result?.research) body.result.research.connection_id = identity;
    await route.fulfill({ response, json: body });
  });
  await page.reload();
  await page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }).click();
  await page.getByLabel("数据源", { exact: true }).selectOption("tushare.ft_mins");
  await page
    .locator(`[data-task-id="handoff-rb-bars"]`)
    .getByRole("button", { name: "用于回测" })
    .click();
  await expect(page.getByRole("button", { name: "下一步", exact: true })).toBeDisabled();
  await page.getByLabel("最小变动价位", { exact: true }).fill("17");
  await page.getByLabel("合约乘数", { exact: true }).fill("99");
  await page.getByLabel("K 线来源", { exact: true }).selectOption(two.source_dataset_ids[0]);
  await expect(page.getByLabel("最小变动价位", { exact: true })).not.toHaveValue("17");
  await expect(page.getByLabel("合约乘数", { exact: true })).not.toHaveValue("99");
  identity = "other-service-with-same-task-ids";
  await expect(
    page.getByText("数据版本不属于当前研究服务，请重新选择数据。", { exact: true }),
  ).toBeVisible();
  await expect(page.getByLabel("K 线来源", { exact: true })).toHaveValue("");
  await expect(page.getByRole("button", { name: "加入组合", exact: true })).toBeDisabled();
  await expect(page.getByRole("button", { name: "下一步", exact: true })).toBeDisabled();
  await page.getByLabel("K 线来源", { exact: true }).selectOption(two.settlement_dataset_ids[0]);
  await expect(page.getByLabel("结算价来源", { exact: true })).toHaveValue(
    two.settlement_dataset_ids[0],
  );
});

test("research prepares a stopped local service once and does not replace an offline remote connection", async ({
  page,
}) => {
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "交易", exact: true }).click();
  await rpc(page.request, "research.local");
  await rpc(page.request, "node.action", { id: "local", service: "research", action: "stop" });
  let offline = true,
    remote = false,
    starts = 0,
    reads = 0;
  await page.route("**/__asterion/api", async route => {
    const method = route.request().postDataJSON().method;
    if (method === "research.local") {
      starts++;
      offline = false;
    }
    if (!["runtime.snapshot", "research.local"].includes(method)) return route.continue();
    const response = await route.fetch();
    const body = await response.json();
    if (body.result?.research) {
      reads++;
      if (offline) body.result.research.online = false;
      body.result.research.remote = remote;
    }
    await route.fulfill({ response, json: body });
  });
  await expect.poll(() => reads).toBeGreaterThan(0);
  await page.locator(".workspace-tabs").getByRole("button", { name: "研究", exact: true }).click();
  await expect.poll(() => starts).toBe(1);
  await expect(page.getByRole("button", { name: "新建回测", exact: true })).toBeEnabled();
  await page.locator(".workspace-tabs").getByRole("button", { name: "交易", exact: true }).click();
  remote = true;
  offline = true;
  await expect.poll(() => reads).toBeGreaterThan(0);
  await page.locator(".workspace-tabs").getByRole("button", { name: "研究", exact: true }).click();
  await expect(
    page.getByText("远程研究连接已断开，当前运行位置保持不变。", { exact: true }),
  ).toBeVisible();
  await expect(page.getByRole("button", { name: "新建回测", exact: true })).toBeDisabled();
  expect(starts).toBe(1);
});

test("a failed preparation waits for explicit retry instead of restarting in a loop", async ({
  page,
}) => {
  await page.goto("/");
  await page.locator(".workspace-tabs").getByRole("button", { name: "交易", exact: true }).click();
  let offline = true,
    attempts = 0,
    reads = 0;
  await page.route("**/__asterion/api", async route => {
    const method = route.request().postDataJSON().method;
    if (method === "research.local") {
      attempts++;
      if (attempts === 1)
        return route.fulfill({
          json: {
            version: 1,
            error: { code: "unavailable", message: "research service is not connected" },
          },
        });
      offline = false;
    }
    if (!["runtime.snapshot", "research.local"].includes(method)) return route.continue();
    const response = await route.fetch();
    const body = await response.json();
    reads++;
    if (body.result?.research) {
      body.result.research.remote = false;
      if (offline) body.result.research.online = false;
    }
    await route.fulfill({ response, json: body });
  });
  await expect.poll(() => reads).toBeGreaterThan(0);
  await page.locator(".workspace-tabs").getByRole("button", { name: "研究", exact: true }).click();
  await expect(page.getByRole("button", { name: "重试准备", exact: true })).toBeVisible();
  const afterFailure = reads;
  await expect.poll(() => reads).toBeGreaterThan(afterFailure + 1);
  expect(attempts).toBe(1);
  await page.getByRole("button", { name: "重试准备", exact: true }).click();
  await expect(page.getByRole("button", { name: "新建回测", exact: true })).toBeEnabled();
  expect(attempts).toBe(2);
});

test("archive versions remain selectable without any download tasks", async ({ page }) => {
  await page.goto("/");
  await seedHistory(page.request, [100, 101], "unrelated-contract", { product: "al" });
  // A product no other spec seeds: the daily settlement is matched
  // automatically only when it is the contract's single candidate.
  const selected = await seedHistory(page.request, [100, 101, 102, 103], "archive-only", {
    product: "sn",
  });
  await page.route("**/__asterion/api", async route => {
    if (route.request().postDataJSON().method !== "runtime.snapshot") return route.continue();
    const response = await route.fetch();
    const body = await response.json();
    if (body.result?.research) body.result.research.tasks = [];
    await route.fulfill({ response, json: body });
  });
  await page.reload();
  await page.locator(".workspace-tabs").getByRole("button", { name: "数据", exact: true }).click();
  await page.getByRole("button", { name: "历史数据仓库", exact: true }).click();
  await page
    .locator(`[data-dataset-id="${selected.source_dataset_ids[0]}"]`)
    .getByRole("button", { name: "用于回测" })
    .click();
  await expect(page.getByLabel("K 线来源", { exact: true })).toHaveValue(
    selected.source_dataset_ids[0],
  );
  await expect(page.getByLabel("结算价来源", { exact: true })).toHaveValue(
    selected.settlement_dataset_ids[0],
  );
  await page.getByLabel("最小变动价位", { exact: true }).fill("1");
  await page.getByLabel("合约乘数", { exact: true }).fill("10");
  await page.getByRole("button", { name: "使用此数据集", exact: true }).click();
  await expect(page.getByRole("list", { name: "已选合约" })).toContainText("4 根 · 1 个交易日");
  await expect(page.getByRole("button", { name: "下一步", exact: true })).toBeEnabled();
  await page.screenshot({
    path: "apps/clients/terminal/test-results/archive-research-selection.png",
  });
});
