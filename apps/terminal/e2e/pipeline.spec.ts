import { fileIdentity } from "./reference-fixture";
import { timeVersion } from "./rules-fixture";
import { expect, test } from "@playwright/test";
test("real worker publishes data and UI restores the task after reload", async ({
  page,
}) => {
  const identity = fileIdentity("SHFE.rb2610");
  const refResponse = await page.request.post("http://127.0.0.1:8000/api/v1/reference/releases", {headers:{Authorization:`Bearer ${process.env.ASTERION_TOKEN}`},data:identity.catalog});
  expect(refResponse.ok()).toBeTruthy();
  const release = await refResponse.json();
  const timeSpec = {...timeVersion.spec, calendar:[{date:"2026-09-14",is_open:true,night_open:true},{date:"2026-09-15",is_open:true,night_open:true}], periods:[{...timeVersion.spec.periods[0],start:"2026-09-15",end:"2026-09-15",night:[{start:"21:00:00",end:"23:00:00",end_offset:0,phase:"continuous"}]}]};
  const timeResponse = await page.request.post("http://127.0.0.1:8000/api/v1/trading-time", {headers:{Authorization:`Bearer ${process.env.ASTERION_TOKEN}`},data:timeSpec});
  expect(timeResponse.ok()).toBeTruthy();
  const time = await timeResponse.json();
  await page.goto("/");
  await page.getByLabel("会话令牌").fill(process.env.ASTERION_TOKEN!);
  await page.locator("form").filter({ has: page.getByLabel("会话令牌") }).getByRole("button", { name: "连接", exact: true }).click();
  await expect(
    page.getByRole("button", { name: "查看服务状态（本机 API可达）" }),
  ).toBeVisible();
  await page
    .getByRole("navigation", { name: "业务工作区" })
    .getByRole("button", { name: "数据", exact: true })
    .click();
  await page.getByRole("button", { name: "导入数据", exact: true }).click();
  await page
    .getByLabel("来源说明")
    .fill("E2E synthetic fixture — not market data");
  await page.getByLabel("导入合约目录").selectOption(release.id);
  await page.getByLabel("文件合约代码 1",{exact:true}).fill("SHFE.rb2610");
  await page.getByLabel("目录来源代码 1",{exact:true}).selectOption(JSON.stringify({source:"tushare",symbol:"RB2610.SHF"}));
  await page.getByLabel("导入数据类型").selectOption("futures.bars");
  await page.getByLabel("交易时间版本",{exact:true}).selectOption(time.id);
  await page.getByLabel("时间戳口径",{exact:true}).selectOption("bar_start");
  await page
    .getByLabel("CSV 数据")
    .fill(
      "contract,event_time,available_at,trading_day,open,high,low,close,volume\nSHFE.rb2610,2026-09-14T13:00:00Z,2026-09-14T13:01:00Z,2026-09-15,3200,3220,3190,3210,100\nSHFE.rb2610,2026-09-14T13:01:00Z,2026-09-14T13:02:00Z,2026-09-15,3210,3230,3200,3220,120",
    );
  await page.getByRole("button", { name: "识别字段并预览" }).click();
  await expect(page.getByText(/校验通过/)).toBeVisible();
  const submitted = page.waitForResponse(
    (r) =>
      r.url().endsWith("/api/v1/imports") && r.request().method() === "POST",
  );
  await page.getByRole("button", { name: "创建采集任务" }).click();
  const job = await (await submitted).json();
  const jobRow = page.getByRole("row").filter({ hasText: job.id.slice(0, 8) });
  await expect(page.getByRole("status").filter({ hasText: "请求已接收" })).toContainText("请求已接收");
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
  await page.locator("form").filter({ has: page.getByLabel("会话令牌") }).getByRole("button", { name: "连接", exact: true }).click();
  await expect(jobRow.getByText("已完成", { exact: true })).toBeVisible();
});

test("invalid token can be corrected without reloading", async ({ page }) => {
  await page.goto("/");
  await page.getByLabel("会话令牌").fill("invalid-token");
  await page.locator("form").filter({ has: page.getByLabel("会话令牌") }).getByRole("button", { name: "连接", exact: true }).click();
  await expect(page.getByRole("alert").filter({ has: page.getByRole("button", { name: "关闭提示" }) })).toContainText(
    "请求身份无效或无权访问此接口",
  );
  await page.getByLabel("会话令牌").fill(process.env.ASTERION_TOKEN!);
  await page.locator("form").filter({ has: page.getByLabel("会话令牌") }).getByRole("button", { name: "连接", exact: true }).click();
  await expect(
    page.getByRole("button", { name: "查看服务状态（本机 API可达）" }),
  ).toBeVisible();
});

test("real rules editor freezes a version but exploration still requires coverage identity", async ({
  page,
}) => {
  const headers = { Authorization: `Bearer ${process.env.ASTERION_TOKEN}` };
  const root = "http://127.0.0.1:8000/api/v1";
  const timeResponse = await page.request.post(`${root}/trading-time`, {headers, data:timeVersion.spec});
  expect(timeResponse.ok()).toBeTruthy();
  const time = await timeResponse.json();
  const identity = fileIdentity();
  const refResponse = await page.request.post(`${root}/reference/releases`, {headers, data:identity.catalog});
  expect(refResponse.ok()).toBeTruthy();
  const release = await refResponse.json();
  const imported = await page.request.post(`${root}/imports`, {
    headers,
    data: {
      command_id: crypto.randomUUID(),
      source: "Offline rule E2E fixture",
      csv: "contract,trading_day,open,high,low,close,vol,settle\nSHFE.rb2405,2024-01-01,10,10,10,10,100,10\nSHFE.rb2405,2024-01-02,10,12,10,12,100,12\nSHFE.rb2405,2024-01-03,20,21,20,21,100,21\nSHFE.rb2405,2024-01-04,22,22,18,18,100,18\nSHFE.rb2405,2024-01-05,17,17,17,17,100,17",
      options: {
        identity: { ...identity, catalog_id: release.id, information_at: new Date().toISOString() },
        type_id: "futures.daily",
        frequency: "1d",
        source_id: "rule_e2e",
      },
    },
  });
  expect(imported.status()).toBe(202);
  const job = await imported.json();
  await expect
    .poll(
      async () =>
        (
          await (
            await page.request.get(`${root}/jobs/${job.id}`, { headers })
          ).json()
        ).state,
    )
    .toBe("SUCCEEDED");
  const catalog = await (
    await page.request.get(
      `${root}/data/catalog?type_id=futures.daily&layer=STANDARD`,
      { headers },
    )
  ).json();
  const daily = catalog.items.find(
    (v: any) => v.manifest.origin.source_id === "rule_e2e",
  );
  expect(daily).toBeTruthy();
  await page.goto("/");
  await page.getByLabel("会话令牌").fill(process.env.ASTERION_TOKEN!);
  await page.locator("form").filter({ has: page.getByLabel("会话令牌") }).getByRole("button", { name: "连接", exact: true }).click();
  await page.getByRole("button", { name: "研究", exact: true }).click();
  await page
    .getByRole("combobox", { name: "日线数据集", exact: true })
    .selectOption(daily.dataset_id);
  await page.getByRole("combobox", { name: "策略", exact: true }).selectOption({ label: "双均线 · 1.0.0" });
  await page.getByLabel("快均线周期", { exact: true }).fill("1");
  await page.getByLabel("慢均线周期", { exact: true }).fill("2");
  await page.getByLabel("初始资金（元）", { exact: true }).fill("1000");
  await page.getByRole("button", { name: "新建规则", exact: true }).click();
  await page.getByLabel("规则合约目录", {exact:true}).selectOption(release.id);
  await page.getByLabel("规则合约", {exact:true}).selectOption(identity.catalog.contracts[0].id);
  await page.getByLabel("交易时间版本",{exact:true}).selectOption(time.id);
  for (const [name, value] of [
    ["规则名称", "真实服务离线测试"],
    ["规则来源与依据", "离线构造，不是交易所数据"],
    ["合约乘数", "10"],
    ["最小变动价位", "1"],
    ["规则开始日期", "2024-01-01"],
    ["规则结束日期", "2024-12-31"],
    ["保证金比例（0—1）", "0.1"],
    ["开仓费用", "2"],
    ["平仓费用", "2"],
  ]) {
    const role = ["保证金比例（0—1）", "开仓费用", "平仓费用"].includes(name)
      ? "spinbutton"
      : "textbox";
    await page.getByRole(role, { name, exact: true }).fill(value);
  }
  const save = page.waitForResponse(
    (r) =>
      r.url().endsWith("/contract-rules") && r.request().method() === "POST",
  );
  await page.getByRole("button", { name: "保存规则版本", exact: true }).click();
  const savedResponse = await save;
  expect(savedResponse.status()).toBe(200);
  const saved = await savedResponse.json();
  await expect(page.getByLabel("固定合约规则")).toContainText(saved.id);
  await page
    .getByRole("combobox", { name: "数据完整性要求", exact: true })
    .selectOption("allow_incomplete");
  await page.getByLabel("探索模式原因").fill("离线测试，没有交易日历依据");
  await page.getByRole("checkbox", { name: /我接受历史收盘/ }).check();
  await expect(page.getByRole("button", { name: "运行回测", exact: true })).toBeDisabled();
  expect(daily.manifest.import_options.identity.catalog_id).toBe(release.id);
  expect(daily.manifest.scope.contract_ids).toEqual(["SHFE.RB.202405.20230516"]);
});
