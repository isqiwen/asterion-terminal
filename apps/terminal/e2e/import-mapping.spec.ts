import { fileIdentity } from "./reference-fixture";
import { expect, test } from "@playwright/test";
import { nativeContext } from "./native";

test("file mapping requires fresh preview and submits explicit provenance", async ({
  context,
  page,
}) => {
  await nativeContext(context);
  const fields = [
    "contract",
    "trading_day",
    "open",
    "high",
    "low",
    "close",
    "vol",
    "amount",
    "oi",
  ];
  const identity = fileIdentity("SHFE.rb2610");
  const release = {id: "a".repeat(64), catalog: identity.catalog, published_at: 1};
  let submitted: any;
  let savedTime: any;
  await context.route("**/api/v1/**", (route) => {
    if (route.request().url().endsWith("/access/scopes")) return route.fulfill({ json: { token: "scope-fixture", expires: Date.now() / 1000 + 300 } });

    const path = new URL(route.request().url()).pathname;
    if (path.endsWith("/reference/releases")) return route.fulfill({json:[{id:release.id,sources:["tushare"],contracts:1,products:1,published_at:1}]});
    if (path.endsWith(`/reference/releases/${release.id}`)) return route.fulfill({json:release});
    if (path.endsWith("/trading-time")) {
      if (route.request().method() === "POST") { savedTime={id:"c".repeat(64),spec:route.request().postDataJSON()}; return route.fulfill({json:savedTime}); }
      return route.fulfill({json:savedTime ? [savedTime] : []});
    }
    if (path.includes("/security")) return route.fallback();
    if (path.endsWith("/imports/preview")) {
      const body = route.request().postDataJSON();
      const valid = body.options.column_mapping.close === "收盘";
      return route.fulfill({
        json: {
          contract_ids: valid ? ["SHFE.RB.202610.20240102"] : [],
          columns: fields.filter((f) => f !== "close").concat("收盘"),
          fields,
          required: fields.slice(0, 7),
          valid,
          total: valid ? 1 : 0,
          errors: valid ? [] : ["缺少收盘字段映射"],
          rows: valid ? [{ contract: "SHFE.rb2610", close: "3210" }] : [],
        },
      });
    }
    if (path.endsWith("/imports")) {
      submitted = route.request().postDataJSON();
      return route.fulfill({
        status: 202,
        json: {
          id: "mapped-import",
          kind: "data.import_csv",
          state: "QUEUED",
          attempt: 0,
          created_at: 1,
        },
      });
    }
    const user = {
      email: "import@example.com",
      first_name: "Import",
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
  await page.getByLabel("邮箱", { exact: true }).fill("import@example.com");
  await page.getByLabel("密码", { exact: true }).fill("import-test-password");
  await page.getByRole("button", { name: "登录", exact: true }).click();
  await page
    .getByRole("navigation", { name: "业务工作区" })
    .getByRole("button", { name: "数据", exact: true })
    .click();
  await page.getByRole("button", { name: "导入数据", exact: true }).click();
  await expect(page.getByRole("button", {name:"识别字段并预览"})).toBeDisabled();
  await page.getByLabel("导入合约目录").selectOption(release.id);
  await page.getByLabel("文件合约代码 1", {exact:true}).fill("SHFE.rb2610");
  await page.getByLabel("目录来源代码 1", {exact:true}).selectOption(JSON.stringify({source:"tushare",symbol:"RB2610.SHF"}));
  await page.getByLabel("来源说明").fill("供应商 A 文件");
  await page
    .getByLabel("CSV 数据")
    .fill(
      "contract,trading_day,open,high,low,收盘,vol\nSHFE.rb2610,2024-01-02,3200,3220,3190,3210,100",
    );
  await page.getByRole("button", {name:"配置交易时间",exact:true}).click();
  const editor=page.getByRole("group", {name:"品种时段与完整日历",exact:true});
  await editor.getByLabel("名称",{exact:true}).fill("本地 RB 时间规则");
  await editor.getByLabel("品种代码",{exact:true}).fill("RB");
  await editor.getByLabel("时段生效起日",{exact:true}).fill("2024-01-02");
  await editor.getByLabel("时段生效止日",{exact:true}).fill("2024-01-02");
  await editor.getByLabel("日历与时段依据",{exact:true}).fill("离线测试时段");
  await editor.getByLabel("夜盘停开依据",{exact:true}).fill("离线测试无夜盘");
  await editor.getByLabel("完整自然日日历",{exact:true}).fill("2024-01-01,1,0\n2024-01-02,1,0");
  await editor.getByRole("button",{name:"保存时间版本",exact:true}).click();
  await expect(page.getByLabel("交易时间版本",{exact:true})).toHaveValue("c".repeat(64));
  const submit = page.getByRole("button", { name: "创建采集任务" });
  await expect(submit).toBeDisabled();
  await page.getByRole("button", { name: "识别字段并预览" }).click();
  await page.getByLabel("映射 close", { exact: true }).selectOption("收盘");
  await expect(submit).toBeDisabled();
  await page.getByRole("button", { name: "识别字段并预览" }).click();
  await expect(submit).toBeEnabled();
  await expect(page.getByLabel("导入实际合约")).toContainText("SHFE.RB.202610.20240102");
  await page.getByLabel("来源标识").fill("vendor_a");
  await expect(submit).toBeDisabled();
  await page.getByRole("button", { name: "识别字段并预览" }).click();
  await expect(submit).toBeEnabled();
  await page.screenshot({ path: "../../.state/import-mapping.png" });
  await submit.click();
  await expect(page.getByRole("status")).toContainText("mapped-import");
  expect(submitted.options.identity.catalog_id).toBe(release.id);
  expect(submitted.options.identity.bindings).toEqual(identity.bindings);
  expect(submitted.options.trading_time).toEqual(savedTime);
  expect(savedTime.spec.calendar[0].night_open).toBe(false);
  expect(submitted.options).toMatchObject({
    type_id: "futures.daily",
    frequency: "1d",
    source_id: "vendor_a",
    column_mapping: { close: "收盘" },
  });
});
