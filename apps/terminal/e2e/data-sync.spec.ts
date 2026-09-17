import { expect, test } from "@playwright/test";
import { nativeContext } from "./native";

const provider = {
  id: "tushare",
  name: "Tushare Pro",
  version: "1.0.0",
  api_version: 1,
  configured: true,
  capabilities: [
    {
      id: "contracts",
      label: "期货合约资料",
      exchanges: ["SHFE", "DCE"],
      date_range: false,
      symbol_required: false,
      description: "普通合约资料",
    },
    {
      id: "calendar",
      label: "期货交易日历",
      exchanges: ["SHFE", "DCE"],
      date_range: true,
      symbol_required: false,
      description: "交易日与休市日",
    },
    {
      id: "daily",
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

test("provider sync submits a credential-free task and previews an immutable release", async ({
  context,
  page,
}) => {
  await nativeContext(context);
  let submitted: Record<string, unknown> | null = null;
  let fail = true;
  const definition = {
    id: "futures.daily",
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
    const path = new URL(route.request().url()).pathname;
    if (path.includes("/account/security")) return route.fallback();
    if (path.endsWith("/data/providers"))
      return route.fulfill({ json: [provider] });
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
      let items = submitted
        ? params.get("layer") === "RAW"
          ? [raw]
          : [release, file]
        : [];
      if (params.get("domain") === "reference") items = [];
      return route.fulfill({ json: { items, total: items.length, offset: 0 } });
    }
    if (path.includes("/data/catalog/") && path.endsWith("/versions")) {
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
  await page.getByLabel("数据类型", { exact: true }).selectOption("daily");
  await page.getByLabel("实际合约代码").fill("RB2610.SHF");
  await page.getByLabel("开始日期").fill("2024-01-02");
  await page.getByLabel("结束日期").fill("2024-01-02");
  await page.getByRole("button", { name: "开始同步" }).click();
  await expect(page.getByRole("alert")).toContainText("接口权限不足");
  await page.getByRole("button", { name: "开始同步" }).click();
  await expect(page.getByRole("status")).toContainText("同步任务已提交");
  expect(submitted).toMatchObject({
    provider: "tushare",
    dataset: "daily",
    symbol: "RB2610.SHF",
    start: "2024-01-02",
    end: "2024-01-02",
  });
  expect(submitted).not.toHaveProperty("token");
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
  ).toHaveText(["数据同步", "数据集", "合约资料"]);
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
  await page.getByLabel("数据源", { exact: true }).selectOption("local_file");
  await expect(page.getByLabel("CSV 数据")).toBeVisible();
  await expect(
    page.getByRole("button", { name: "开始同步", exact: true }),
  ).toHaveCount(0);
  await page.getByRole("button", { name: "查看数据集", exact: true }).click();
  await expect(
    page.getByRole("cell", { name: "本地文件测试来源", exact: true }),
  ).toBeVisible();
});

test("data source settings keep credentials out of persistent frontend storage", async ({
  context,
  page,
}) => {
  await nativeContext(context);
  let configured = false;
  let saved = "";
  await context.route("**/api/v1/**", (route) => {
    const path = new URL(route.request().url()).pathname;
    if (path.includes("/account/security")) return route.fallback();
    if (path.endsWith("/data/providers"))
      return route.fulfill({ json: [{ ...provider, configured }] });
    if (path.endsWith("/credential")) {
      saved = route.request().postDataJSON().token;
      configured = !!saved;
      return route.fulfill({ json: { configured } });
    }
    if (path.endsWith("/check"))
      return route.fulfill({ json: { message: "交易日历接口验证通过" } });
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
  await page.getByRole("button", { name: "保存 Token" }).click();
  await expect(field).toHaveValue("");
  expect(saved).toBe("synthetic-only-provider-secret");
  expect(
    await page.evaluate(
      () => JSON.stringify(localStorage) + JSON.stringify(sessionStorage),
    ),
  ).not.toContain(saved);
  await page.getByRole("button", { name: "测试已保存的凭据" }).click();
  await expect(page.getByRole("status")).toContainText("验证通过");
  await page.getByRole("button", { name: "移除凭据" }).click();
  await expect(page.getByRole("status")).toContainText("已移除");
  await expect(
    page.getByRole("button", { name: "测试已保存的凭据" }),
  ).toBeDisabled();
});

test("data source shortcut selects its settings tab on creation and window reuse", async ({
  context,
  page,
}) => {
  await nativeContext(context);
  await context.route("**/api/v1/**", (route) => {
    const path = new URL(route.request().url()).pathname;
    if (path.includes("/account/security")) return route.fallback();
    return route.fulfill({
      json: path.endsWith("/data/providers")
        ? [provider]
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
