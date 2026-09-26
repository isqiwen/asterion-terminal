import { providerFixture } from "./provider-fixture";
import { expect, test } from "@playwright/test";
import { nativeContext } from "./native";

const provider = {
  id: "tushare",
  name: "Tushare Pro",
  version: "1.0.0",
  api_version: 2,
  configured: true,
  description: "Tushare 历史期货数据",
  demo: false,
  configuration: {
    schema_version: 1,
    fields: [
      {
        id: "token",
        label: "Tushare Token",
        type: "string",
        secret: true,
        required: true,
        description: "Tushare Pro 接口凭据",
        placeholder: "输入 Token",
        max_length: 256,
      },
    ],
  },
  capabilities: [
    {id: "minute", frequencies:["1m","5m","15m","30m","60m"], type_id: "futures.minute", label: "一分钟", exchanges: ["SHFE"], date_range: true, symbol_required: true, description: "实际合约分钟"},
    {
      id: "contracts",
      type_id: "futures.contracts",
      label: "期货合约资料",
      exchanges: ["SHFE", "DCE"],
      date_range: false,
      symbol_required: false,
      description: "普通合约资料",
    },
    {
      id: "calendar",
      type_id: "futures.calendar",
      label: "期货交易日历",
      exchanges: ["SHFE", "DCE"],
      date_range: true,
      symbol_required: false,
      description: "交易日与休市日",
    },
    {
      id: "settlement", type_id: "futures.settlement", label: "结算参数",
      exchanges: ["SHFE", "DCE"], date_range: true, symbol_required: true,
      description: "实际合约结算参数",
    },
    {
      id: "daily",
      type_id: "futures.daily",
      label: "期货历史日线",
      exchanges: ["SHFE", "DCE"],
      date_range: true,
      symbol_required: true,
      description: "实际合约日线",
    },
  ],
};
const user = {
  email: "data-test@example.com",
  first_name: "Data",
  last_name: "Test",
};

for (const selection of ["daily", "settlement", "1m", "5m", "15m", "30m", "60m"]) {
const dataset = selection.endsWith("m") ? "minute" : selection;
test(`${selection} sync pins contract evidence and previews an immutable release`, async ({
  context,
  page,
}) => {
  await nativeContext(context);
  let submitted: Record<string, unknown> | null = null;
  let fail = true;
  const definition = {
    id: `futures.${dataset}`,
    label: "历史日线",
    domain: "market",
    domain_label: "行情",
    shape: "timeseries",
    frequency: "1d",
    schema_version: 1,
    primary_key: ["contract", "trading_day"],
    time_semantics: "交易日标签",
    description: "实际合约日线",
    fields: [{ name: "settle", label: "结算", unit: null }],
  };
  const release = {
    id: "version-001",
    dataset_id: "dataset-001",
    job_id: "sync-job",
    created_at: 1789600000,
    rows: 1,
    version_count: 2,
    manifest: {
      type: definition,
      source: "tushare",
      layer: "STANDARD",
      scope: { symbol: "RB2610.SHF", exchange: "SHFE" },
      format: "parquet",
      bytes: 1024,
      checksum: "fixture",
      inputs: ["raw-001"],
      first: "2024-01-02",
      last: "2024-01-02",
      coverage: "RETURNED_ROWS_ONLY",
      quality: "VALIDATED",
      partitions: [{key: "2024-01-02"}],
      transform: { id: "normalize:tushare", version: "1.0.0" },
    },
  };
  const older = { ...release, id: "version-000", created_at: 1789500000 };
  const raw = {
    ...release,
    id: "raw-001",
    dataset_id: "raw-dataset",
    manifest: {
      ...release.manifest,
      layer: "RAW",
      format: "provider_evidence",
      inputs: [],
      transform: null,
    },
  };
  const file = {
    ...release,
    id: "file-001",
    dataset_id: "file-dataset",
    manifest: {
      ...release.manifest,
      source: "local_file",
      scope: { name: "本地文件测试来源" },
      type: {
        ...definition,
        id: "futures.bars",
        label: "历史行情 / 文件",
        frequency: "unspecified",
      },
    },
  };
  const job = {
    id: "sync-job",
    command_id: "fixture",
    kind: "data.sync",
    state: "SUCCEEDED",
    attempt: 1,
    created_at: 1789600000,
    result: { dataset_id: release.id, completed: 1, total: 1 },
  };
  await context.route("**/api/v1/**", (route) => {
    if (route.request().url().endsWith("/access/scopes")) return route.fulfill({ json: { token: "scope-fixture", expires: Date.now() / 1000 + 300 } });

    const path = new URL(route.request().url()).pathname;
    if (path.endsWith("/trading-time")) return route.fulfill({json: [{id:"time-fixed", spec:{exchange:"SHFE",product:"RB",title:"测试时间依据",timezone:"Asia/Shanghai",calendar_source:"测试日历",night_source:"测试夜盘",periods:[{start:"2024-01-01",end:"2024-01-31"}]}}]});
    if (path.endsWith("/minute-coverage")) return route.fulfill({json: {version_id: "version-001", checksum:"fixture", contract_id:"SHFE.RB.202610.fixture",trading_day:"2024-01-02",trading_time_id:"time-fixed",frequency:selection,status:selection === "1m" ? "GAPS" : "UNVERIFIED",expected:selection === "1m" ? 225 : null,present:224,missing:selection === "1m" ? ["2024-01-02T09:01:00+08:00"] : null,note:"缺少记录不自动补零"}});
    if (path.includes("/account/security")) return route.fallback();
    if (path.endsWith("/data/providers"))
      return route.fulfill({ json: [providerFixture(provider)] });
    if (path.endsWith("/data/sync")) {
      if (fail) {
        fail = false;
        return route.fulfill({
          status: 422,
          json: { detail: "测试：接口权限不足" },
        });
      }
      submitted = route.request().postDataJSON();
      return route.fulfill({ status: 202, json: job });
    }
    if (path.endsWith("/data/types"))
      return route.fulfill({
        json: [
          definition,
          {
            ...definition,
            id: "futures.calendar",
            label: "交易日历",
            domain: "reference",
            domain_label: "基础资料",
          },
        ],
      });
    if (path.endsWith("/data/catalog")) {
      const params = new URL(route.request().url()).searchParams;
      if (params.get("type_id") === "futures.contracts") return route.fulfill({json: {items: [{...release, id: "contracts-fixed", dataset_id: "contracts-set"}], total: 1}});
      let items = submitted
        ? params.get("layer") === "RAW"
          ? [raw]
          : [release, file]
        : [];
      if (params.get("domain") === "reference") items = [];
      return route.fulfill({ json: { items, total: items.length, offset: 0 } });
    }
    if (path.includes("/data/catalog/") && path.endsWith("/versions")) {
      if (path.includes("contracts-set")) return route.fulfill({json: {items: [{...release, id: "contracts-fixed"}], total: 1}});
      const items = path.includes("raw-dataset") ? [raw] : [release, older];
      return route.fulfill({ json: { items, total: items.length, offset: 0 } });
    }
    if (path.includes("/data/versions/")) {
      const version = path.endsWith(raw.id)
        ? raw
        : path.endsWith(older.id)
          ? older
          : release;
      return route.fulfill({
        json: {
          version,
          offset: 0,
          total: 1,
          snapshot: null,
          rows: [
            {
              symbol: "RB2610.SHF",
              trading_day: "2024-01-02",
              close: "3210",
              settle: version.id === older.id ? "3204" : "3205",
            },
          ],
        },
      });
    }
    if (path.endsWith("/snapshots"))
      return route.fulfill({
        json: [
          {
            id: "file-fixture-001",
            job_id: "file-job",
            manifest: {
              source: "本地文件测试来源",
              contracts: ["SHFE.rb2610"],
              rows: 2,
              start: "2024-01-02",
              end: "2024-01-03",
            },
          },
        ],
      });
    if (path.endsWith("/jobs"))
      return route.fulfill({ json: submitted ? [job] : [] });
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
    .fill("synthetic-test-password");
  await page.getByRole("button", { name: "登录", exact: true }).click();
  await page
    .getByRole("navigation", { name: "业务工作区" })
    .getByRole("button", { name: "数据", exact: true })
    .click();
  await page.getByLabel("数据类型", { exact: true }).selectOption(dataset);
  await page.getByLabel("来源代码").fill("RB2610.SHF");
  await page.getByLabel("开始日期").fill("2024-01-02");
  if (dataset === "minute") {
    await expect(page.getByLabel("结束日期")).toHaveCount(0);
    await expect(page.getByRole("button", {name: "开始同步"})).toBeDisabled();
    await page.getByLabel("交易时间版本", {exact:true}).selectOption("time-fixed");
    await page.getByLabel("分钟周期").selectOption(selection);
    await page.getByLabel("分钟时间戳含义").selectOption("bar_end");
    await page.getByLabel("分钟时间含义依据").fill("合成测试依据");
  } else await page.getByLabel("结束日期").fill("2024-01-02");
  await page.getByLabel("同步合约资料版本").selectOption("contracts-fixed");
  await page.getByLabel("来源代码").fill("rb2610.SHF");
  await expect(page.getByLabel("来源代码")).toHaveValue("rb2610.SHF");
  await expect(page.getByLabel("同步合约资料版本")).toHaveValue("");
  await page.getByLabel("来源代码").fill("RB2610.SHF");
  await page.getByLabel("同步合约资料版本").selectOption("contracts-fixed");
  await page.getByRole("button", { name: "开始同步" }).click();
  await expect(page.getByRole("alert")).toContainText("接口权限不足");
  await page.getByRole("button", { name: "开始同步" }).click();
  await expect(page.getByRole("status").filter({hasText:"同步任务已提交"})).toBeVisible();
  expect(submitted).toMatchObject({
    provider: "tushare",
    contracts_version_id: "contracts-fixed",
    dataset,
    symbol: "RB2610.SHF",
    start: "2024-01-02",
    end: "2024-01-02",
  });
  expect(submitted).not.toHaveProperty("token");
  if (dataset === "minute") expect(submitted).toMatchObject({minute_context:{frequency:selection,trading_time:{id:"time-fixed"},timestamp_semantics:"bar_end",semantics_source:"合成测试依据"}});
  await page.getByRole("button", { name: "查看数据集", exact: true }).click();
  await page.getByRole("button", { name: "历史日线", exact: true }).click();
  await expect(
    page.getByRole("cell", { name: "3205", exact: true }),
  ).toBeVisible();
  await expect(
    page.getByText("仅已返回记录", { exact: true }).first(),
  ).toBeVisible();
  await expect(page.getByText("数据集已发布", { exact: true })).toBeVisible();
  await expect(
    page.getByRole("cell", { name: "本地文件测试来源", exact: true }),
  ).toBeVisible();
  await expect(
    page.getByRole("navigation", { name: "业务内分类" }).getByRole("button"),
  ).toHaveText(["数据同步", "数据集", "合约资料", "合约角色"]);
  if (dataset === "minute") {
    await page.getByRole("button", {name:"核对分钟覆盖"}).click();
    await expect(page.getByRole("status").filter({hasText:selection === "1m" ? "存在缺失分钟" : "完整性待核验"})).toBeVisible();
    await page.getByText("缺失时间与固定依据").click();
    if (selection === "1m") await expect(page.getByText("2024-01-02T09:01:00+08:00", {exact:true})).toBeVisible();
  }
  await page.getByLabel("采集版本").selectOption(older.id);
  await expect(
    page.getByRole("cell", { name: "3204", exact: true }),
  ).toBeVisible();
  await page.getByRole("button", { name: "查看原始输入" }).click();
  await expect(
    page.getByRole("heading", { name: "历史日线 · 原始预览" }),
  ).toBeVisible();
  await page.getByLabel("业务类别").selectOption("reference");
  await expect(
    page.getByText("没有符合条件的数据集。", { exact: false }),
  ).toBeVisible();
  await page.getByLabel("业务类别").selectOption("");
  await page.getByLabel("加工阶段").selectOption("RAW");
  await expect(
    page.getByRole("cell", { name: "原始", exact: true }),
  ).toBeVisible();
  await page.getByLabel("加工阶段").selectOption("STANDARD");
  await page.screenshot({ path: "../../.state/data-sync-ui.png" });
  await page.getByRole("button", { name: "数据同步", exact: true }).click();
  await page.getByRole("button", { name: "导入数据", exact: true }).click();
  await expect(page.getByLabel("CSV 数据")).toBeVisible();
  await expect(
    page.getByRole("button", { name: "开始同步", exact: true }),
  ).toHaveCount(0);
  await page.getByRole("button", { name: "查看数据集", exact: true }).click();
  await expect(
    page.getByRole("cell", { name: "本地文件测试来源", exact: true }),
  ).toBeVisible();
});

}

test("data source settings keep credentials out of persistent frontend storage", async ({
  context,
  page,
}) => {
  await nativeContext(context);
  let configured = false;
  let saved = "";
  let revision = 0;
  await context.route("**/api/v1/**", (route) => {
    if (route.request().url().endsWith("/access/scopes")) return route.fulfill({ json: { token: "scope-fixture", expires: Date.now() / 1000 + 300 } });

    const path = new URL(route.request().url()).pathname;
    if (path.includes("/account/security")) return route.fallback();
    if (path.endsWith("/data/providers"))
      return route.fulfill({ json: [providerFixture({ ...provider, configured })] });
    if (path.endsWith("/configuration")) {
      if (route.request().method() === "POST") {
        const body = route.request().postDataJSON();
        expect(body.expected_revision).toBe(revision);
        if (Object.hasOwn(body.secrets, "token"))
          saved = body.secrets.token ?? "";
        configured = !!saved;
        revision++;
      }
      return route.fulfill({
        json: {
          provider: "tushare",
          revision,
          schema_version: 1,
          values: {},
          secret_fields: configured ? ["token"] : [],
          configured,
        },
      });
    }
    if (path.endsWith("/configuration/check"))
      return route.fulfill({
        json: { status: "verified", message: "交易日历接口验证通过", revision },
      });
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
  await page.goto("/?screen=settings");
  await page.getByLabel("邮箱", { exact: true }).fill(user.email);
  await page
    .getByLabel("密码", { exact: true })
    .fill("synthetic-test-password");
  await page.getByRole("button", { name: "登录", exact: true }).click();
  await page.getByRole("button", { name: "数据源", exact: true }).click();
  const field = page.getByLabel("Tushare Token", { exact: true });
  await expect(field).toHaveAttribute("type", "password");
  await field.fill("synthetic-only-provider-secret");
  await page.getByRole("button", { name: "保存配置", exact: true }).click();
  await expect(field).toHaveValue("");
  expect(saved).toBe("synthetic-only-provider-secret");
  expect(
    await page.evaluate(
      () => JSON.stringify(localStorage) + JSON.stringify(sessionStorage),
    ),
  ).not.toContain(saved);
  await page.getByRole("button", { name: "测试当前配置" }).click();
  await expect(page.getByRole("status")).toContainText("验证通过");
  await page.getByRole("button", { name: "移除已存凭据" }).click();
  await page.getByRole("button", { name: "保存配置", exact: true }).click();
  await expect(page.getByRole("status")).toContainText("配置已保存");
  expect(saved).toBe("");
  await expect(
    page.getByRole("button", { name: "测试当前配置" }),
  ).toBeDisabled();
});

test("data source shortcut selects its settings tab on creation and window reuse", async ({
  context,
  page,
}) => {
  await nativeContext(context);
  await context.route("**/api/v1/**", (route) => {
    if (route.request().url().endsWith("/access/scopes")) return route.fulfill({ json: { token: "scope-fixture", expires: Date.now() / 1000 + 300 } });

    const path = new URL(route.request().url()).pathname;
    if (path.includes("/account/security")) return route.fallback();
    if (path.endsWith("/configuration"))
      return route.fulfill({
        json: {
          provider: "tushare",
          revision: 1,
          schema_version: 1,
          values: {},
          secret_fields: ["token"],
          configured: true,
        },
      });
    return route.fulfill({
      json: path.endsWith("/data/providers")
        ? [providerFixture(provider)]
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
  await page.getByLabel("邮箱", { exact: true }).fill(user.email);
  await page
    .getByLabel("密码", { exact: true })
    .fill("synthetic-test-password");
  await page.getByRole("button", { name: "登录", exact: true }).click();
  await page
    .getByRole("navigation", { name: "业务工作区" })
    .getByRole("button", { name: "数据", exact: true })
    .click();
  const created = context.waitForEvent("page");
  await page.getByRole("button", { name: "数据源设置", exact: true }).click();
  const settings = await created;
  await expect(
    settings.getByRole("heading", { name: "数据源", exact: true }),
  ).toBeVisible();
  await expect(
    settings.getByLabel("Tushare Token", { exact: true }),
  ).toBeVisible();
  await settings.getByRole("button", { name: "外观", exact: true }).click();
  await expect(
    settings.getByRole("heading", { name: "外观", exact: true }),
  ).toBeVisible();
  await page.getByRole("button", { name: "数据源设置", exact: true }).click();
  await expect(
    settings.getByRole("heading", { name: "数据源", exact: true }),
  ).toBeVisible();
  expect(context.pages()).toHaveLength(2);
  await settings.close();
  const reopened = context.waitForEvent("page");
  await page.getByRole("button", { name: "数据源设置", exact: true }).click();
  await expect(
    (await reopened).getByRole("heading", { name: "数据源", exact: true }),
  ).toBeVisible();
});
