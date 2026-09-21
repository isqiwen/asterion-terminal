import { providerFixture } from "./provider-fixture";
import { expect, test, type Page } from "@playwright/test";
import { nativeContext } from "./native";

const user = {
  email: "plugins@example.com",
  first_name: "Plugin",
  last_name: "Developer",
};
const daily = {
  id: "daily",
  label: "期货历史日线",
  exchanges: ["SIM"],
  date_range: true,
  symbol_required: true,
  description: "合成日线",
  defaults: { symbol: "DEMO001.SIM", start: "2024-01-02", end: "2024-01-12" },
};
const synthetic = {
  id: "synthetic",
  name: "合成示例（非真实行情）",
  version: "1.0.0",
  api_version: 2,
  configured: true,
  demo: true,
  description: "无需凭据的可重复示例。",
  configuration: {
    schema_version: 1,
    fields: [
      {
        id: "seed",
        label: "生成种子",
        type: "integer",
        secret: false,
        required: true,
        default: 7,
        minimum: 0,
        maximum: 10000,
        description: "相同配置生成相同数据。",
        placeholder: "7",
      },
    ],
  },
  capabilities: [
    daily,
    {
      id: "calendar",
      label: "期货交易日历",
      exchanges: ["SIM"],
      date_range: true,
      symbol_required: false,
      description: "合成日历",
      defaults: { start: "2024-01-01", end: "2024-01-31" },
    },
  ],
};
const development = {
  ...synthetic,
  id: "development",
  name: "开发数据源",
  demo: false,
  configured: false,
  description: "第三方数据源配置示例。",
  configuration: {
    schema_version: 1,
    fields: [
      {
        id: "access_key",
        label: "访问密钥",
        type: "string",
        secret: true,
        required: true,
        min_length: 8,
        max_length: 100,
        description: "数据源专用密钥",
        placeholder: "输入密钥",
      },
      {
        id: "label",
        label: "连接名称",
        type: "string",
        secret: false,
        required: false,
        default: "研究连接",
        max_length: 20,
        description: "本机连接备注",
        placeholder: "可选",
      },
      {
        id: "enabled",
        label: "启用压缩",
        type: "boolean",
        secret: false,
        required: true,
        default: false,
        description: "测试布尔配置",
        placeholder: "",
      },
    ],
  },
};
async function login(page: Page, path = "/") {
  await page.goto(path);
  await page.getByLabel("邮箱", { exact: true }).fill(user.email);
  await page
    .getByLabel("密码", { exact: true })
    .fill("synthetic-test-password");
  await page.getByRole("button", { name: "登录", exact: true }).click();
}

test("schema-driven provider drafts stay isolated and survive revision conflicts", async ({
  context,
  page,
}) => {
  await nativeContext(context);
  let state = {
    provider: "synthetic",
    revision: 0,
    schema_version: 1,
    values: { seed: 7 },
    secret_fields: [],
    configured: true,
  };
  const developmentState = {
    provider: "development",
    revision: 0,
    schema_version: 1,
    values: { label: "研究连接", enabled: false },
    secret_fields: [] as string[],
    configured: false,
  };
  const checks: Record<string, unknown>[] = [];
  const writes: { provider: string; body: Record<string, any> }[] = [];
  let conflict = true;
  await context.route("**/api/v1/**", async (route) => {
    if (route.request().url().endsWith("/access/scopes")) return route.fulfill({ json: { token: "scope-fixture", expires: Date.now() / 1000 + 300 } });

    const path = new URL(route.request().url()).pathname;
    if (path.includes("/account/security")) return route.fallback();
    if (path.endsWith("/data/providers"))
      return route.fulfill({ json: [synthetic, development].map(providerFixture) });
    if (path.endsWith("/configuration/check")) {
      checks.push(route.request().postDataJSON());
      return route.fulfill({
        json: {
          status: "verified",
          message: "示例生成验证通过",
          revision: state.revision,
        },
      });
    }
    if (path.endsWith("/configuration")) {
      const provider = path.split("/").at(-2)!;
      if (route.request().method() === "POST") {
        const body = route.request().postDataJSON();
        writes.push({ provider, body });
        if (provider === "synthetic") {
          if (conflict) {
            conflict = false;
            state = { ...state, revision: 1, values: { seed: 9 } };
            return route.fulfill({
              status: 409,
              json: { detail: "配置已在其他窗口修改，请刷新后再保存" },
            });
          }
          expect(body.expected_revision).toBe(1);
          expect(body.secrets).toEqual({});
          state = { ...state, revision: 2, values: body.values };
        } else {
          expect(body.values).toEqual({ label: "开发连接", enabled: true });
          developmentState.revision++;
          developmentState.configured = true;
          developmentState.secret_fields = ["access_key"];
          developmentState.values = body.values;
        }
      }
      return route.fulfill({
        json: provider === "synthetic" ? state : developmentState,
      });
    }
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
  await login(page, "/?screen=settings");
  await page.getByRole("button", { name: "数据源", exact: true }).click();
  const demo = page.getByRole("region", { name: synthetic.name, exact: true });
  const other = page.getByRole("region", {
    name: development.name,
    exact: true,
  });
  const seed = demo.getByLabel("生成种子", { exact: true });
  const key = other.getByLabel("访问密钥", { exact: true });
  await expect(seed).toHaveValue("7");
  await expect(
    demo.getByText("合成示例 · 非真实行情。", { exact: true }),
  ).toBeVisible();
  await expect(
    other.getByRole("button", { name: "测试当前配置" }),
  ).toBeDisabled();
  await key.fill("only-development-draft-secret");
  await other.getByLabel("连接名称", { exact: true }).fill("开发连接");
  await other.getByLabel("启用压缩", { exact: true }).check();
  await seed.fill("10001");
  await demo.getByRole("button", { name: "测试当前配置" }).click();
  expect(
    await seed.evaluate(
      (input: HTMLInputElement) => input.validity.rangeOverflow,
    ),
  ).toBe(true);
  expect(checks).toHaveLength(0);
  await seed.fill("11");
  await demo.getByRole("button", { name: "测试当前配置" }).click();
  await expect(demo.getByRole("status")).toContainText("草稿尚未应用");
  await expect(other.getByRole("status")).toHaveCount(0);
  expect(checks).toEqual([
    { expected_revision: 0, values: { seed: 11 }, secrets: {} },
  ]);
  expect(writes).toHaveLength(0);
  expect(state.values.seed).toBe(7);
  await demo.getByRole("button", { name: "保存配置", exact: true }).click();
  await expect(demo.getByRole("alert")).toContainText("其他窗口修改");
  await expect(seed).toHaveValue("11");
  await expect(key).toHaveValue("only-development-draft-secret");
  await expect(other.getByRole("alert")).toHaveCount(0);
  await demo
    .getByRole("button", { name: "刷新已保存配置（覆盖草稿）", exact: true })
    .click();
  await expect(seed).toHaveValue("9");
  await seed.fill("12");
  await demo.getByRole("button", { name: "保存配置", exact: true }).click();
  await expect(demo.getByRole("status")).toContainText("尚未验证");
  await expect(key).toHaveValue("only-development-draft-secret");
  await other.getByRole("button", { name: "保存配置", exact: true }).click();
  await expect(key).toHaveValue("");
  expect(writes.at(-1)?.body.secrets).toEqual({
    access_key: "only-development-draft-secret",
  });
  expect(
    await page.evaluate(
      () => JSON.stringify(localStorage) + JSON.stringify(sessionStorage),
    ),
  ).not.toContain("only-development-draft-secret");
  await expect(demo.getByRole("status")).toContainText("尚未验证");
  await page.screenshot({ path: "../../.state/provider-configuration.png" });
});

test("synthetic sync uses capability defaults without credentials and preserves retry commands", async ({
  context,
  page,
}) => {
  await nativeContext(context);
  const submissions: Record<string, unknown>[] = [];
  const definition = {
    id: "futures.daily",
    label: "期货历史日线",
    domain: "market",
    domain_label: "行情",
    shape: "timeseries",
    frequency: "1d",
    schema_version: 1,
    primary_key: ["contract", "trading_day"],
    time_semantics: "交易日标签",
    description: "实际合约日线",
    fields: [{ name: "close", label: "收盘", unit: null }],
  };
  const release = {
    id: "synthetic-version",
    dataset_id: "synthetic-dataset",
    job_id: "synthetic-job",
    created_at: 1789600000,
    rows: 1,
    version_count: 1,
    manifest: {
      demo: true,
      source: "synthetic",
      type: definition,
      layer: "STANDARD",
      scope: { symbol: "DEMO001.SIM", exchange: "SIM" },
      format: "parquet",
      checksum: "fixture",
      bytes: 1024,
      inputs: [],
      first: "2024-01-02",
      last: "2024-01-02",
      coverage: "RETURNED_ROWS_ONLY",
      quality: "VALIDATED",
      transform: null,
      version_semantics: "CUMULATIVE",
      revision: 1,
      acquired_rows: 1,
    },
  };
  const job = {
    id: "synthetic-job",
    command_id: "fixture",
    kind: "data.sync",
    state: "SUCCEEDED",
    attempt: 1,
    created_at: 1789600000,
    result: { dataset_id: release.id },
  };
  await context.route("**/api/v1/**", (route) => {
    if (route.request().url().endsWith("/access/scopes")) return route.fulfill({ json: { token: "scope-fixture", expires: Date.now() / 1000 + 300 } });

    const path = new URL(route.request().url()).pathname;
    if (path.includes("/account/security")) return route.fallback();
    if (path.endsWith("/data/providers"))
      return route.fulfill({ json: [development, synthetic].map(providerFixture) });
    if (path.endsWith("/jobs"))
      return route.fulfill({ json: submissions.length >= 3 ? [job] : [] });
    if (path.endsWith("/data/types"))
      return route.fulfill({ json: [definition] });
    if (path.endsWith("/coverage")) return route.fulfill({ json: null });
    if (path.endsWith(`/data/versions/${release.id}`))
      return route.fulfill({
        json: {
          version: release,
          rows: [
            {
              contract: "SIM.DEMO001",
              trading_day: "2024-01-02",
              close: "101",
            },
          ],
          total: 1,
          offset: 0,
          snapshot: null,
        },
      });
    if (
      path.endsWith("/data/catalog") ||
      path.endsWith(`/data/catalog/${release.dataset_id}/versions`)
    )
      return route.fulfill({ json: { items: [release], total: 1, offset: 0 } });
    if (path.endsWith("/data/sync")) {
      submissions.push(route.request().postDataJSON());
      if (submissions.length < 3)
        return route.fulfill({
          status: 503,
          json: { detail: "连接中断，请重试" },
        });
      return route.fulfill({ status: 202, json: job });
    }
    return route.fulfill({
      json: path.endsWith("/login")
        ? { session: "session", user }
        : path.endsWith("/me")
          ? user
          : path.endsWith("/health")
            ? { status: "ready" }
            : path.endsWith("/data/catalog")
              ? { items: [], total: 0, offset: 0 }
              : [],
    });
  });
  await login(page);
  await page
    .getByRole("navigation", { name: "业务工作区" })
    .getByRole("button", { name: "数据", exact: true })
    .click();
  await expect(page.getByLabel("数据源", { exact: true })).toHaveValue(
    "synthetic",
  );
  await page.getByLabel("数据源", { exact: true }).selectOption("development");
  await expect(
    page.getByRole("button", { name: "开始同步", exact: true }),
  ).toBeDisabled();
  await page.getByLabel("数据源", { exact: true }).selectOption("synthetic");
  await expect(page.getByLabel("数据类型", { exact: true })).toHaveValue(
    "daily",
  );
  await expect(page.getByLabel("交易所", { exact: true })).toHaveValue("SIM");
  await expect(page.getByLabel("来源代码", { exact: true })).toHaveValue(
    "DEMO001.SIM",
  );
  await expect(page.getByLabel("开始日期", { exact: true })).toHaveValue(
    "2024-01-02",
  );
  await expect(page.getByLabel("结束日期", { exact: true })).toHaveValue(
    "2024-01-12",
  );
  await expect(
    page.getByText("合成示例 · 非真实行情。", { exact: true }),
  ).toBeVisible();
  await page.getByLabel("数据类型", { exact: true }).selectOption("calendar");
  await expect(page.getByLabel("开始日期", { exact: true })).toHaveValue(
    "2024-01-01",
  );
  await expect(page.getByLabel("结束日期", { exact: true })).toHaveValue(
    "2024-01-31",
  );
  await expect(page.getByLabel("来源代码", { exact: true })).toHaveCount(0);
  await page.getByLabel("数据类型", { exact: true }).selectOption("daily");
  await page.getByRole("button", { name: "开始同步", exact: true }).click();
  await expect(page.getByRole("alert")).toContainText("连接中断");
  await page.getByRole("button", { name: "开始同步", exact: true }).click();
  await expect(page.getByRole("alert")).toContainText("连接中断");
  expect(submissions[0]).toEqual(submissions[1]);
  expect(submissions[0]).toMatchObject({
    provider: "synthetic",
    dataset: "daily",
    exchange: "SIM",
    symbol: "DEMO001.SIM",
    start: "2024-01-02",
    end: "2024-01-12",
  });
  expect(submissions[0]).not.toHaveProperty("secrets");
  expect(submissions[0]).not.toHaveProperty("token");
  await page.getByLabel("结束日期", { exact: true }).fill("2024-01-11");
  await page.getByRole("button", { name: "开始同步", exact: true }).click();
  await expect(page.getByRole("status")).toContainText("同步任务已提交");
  expect(submissions[2].command_id).not.toBe(submissions[1].command_id);
  await expect(
    page.getByRole("cell", { name: "数据源同步", exact: true }),
  ).toBeVisible();
  await page.getByRole("button", { name: "查看数据集", exact: true }).click();
  const source = page.getByRole("cell", {
    name: "合成示例 · 非真实行情",
    exact: true,
  });
  await expect(source).toBeVisible();
  await expect(source).toHaveAttribute("title", "synthetic");
  await page.getByRole("button", { name: "期货历史日线", exact: true }).click();
  await expect(
    page.getByRole("heading", {
      name: "期货历史日线 · 标准化预览 · 合成示例 · 非真实行情",
      exact: true,
    }),
  ).toBeVisible();
});

test("synthetic charts keep their non-market label when detached", async ({
  context,
  page,
}) => {
  await nativeContext(context);
  const bars = [
    {
      contract: "SIM.DEMO001",
      event_time: "2024-01-02T00:00:00Z",
      open: "100",
      high: "102",
      low: "99",
      close: "101",
      volume: 50,
    },
  ];
  const snapshot = {
    id: "demo-snapshot",
    manifest: {
      demo: true,
      source: "合成示例（非真实行情）",
      frequency: "1d",
      contracts: ["SIM.DEMO001"],
      rows: 1,
      start: "2024-01-02",
      end: "2024-01-02",
    },
  };
  await context.route("**/api/v1/**", (route) => {
    if (route.request().url().endsWith("/access/scopes")) return route.fulfill({ json: { token: "scope-fixture", expires: Date.now() / 1000 + 300 } });

    const path = new URL(route.request().url()).pathname;
    if (path.includes("/account/security")) return route.fallback();
    return route.fulfill({
      json: path.endsWith("/login")
        ? { session: "session", user }
        : path.endsWith("/me")
          ? user
          : path.endsWith("/health")
            ? { status: "ready" }
            : path.endsWith("/snapshots")
              ? [snapshot]
              : path.endsWith("/bars")
                ? bars
                : [],
    });
  });
  await login(page);
  await expect(page.locator(".chart canvas").first()).toBeVisible();
  await expect(
    page.getByText("合成示例 · 非真实行情", { exact: true }),
  ).toBeVisible();
  const created = context.waitForEvent("page");
  await page.getByRole("button", { name: "拆出图表", exact: true }).click();
  const detached = await created;
  await expect(detached.locator(".chart canvas").first()).toBeVisible();
  await expect(
    detached.getByText("合成示例 · 非真实行情", { exact: true }),
  ).toBeVisible();
});

test("a damaged provider configuration can be replaced without retaining stale errors", async ({
  context,
  page,
}) => {
  await nativeContext(context);
  const damaged = { ...development, credential_error: "旧状态：配置不可读" };
  let state = {
    provider: "development",
    revision: 3,
    schema_version: 1,
    values: { label: "研究连接", enabled: false },
    secret_fields: [] as string[],
    configured: false,
    error: "配置快照损坏，请重新填写完整配置" as string | null,
  };
  let submitted: Record<string, unknown> | null = null;
  await context.route("**/api/v1/**", (route) => {
    if (route.request().url().endsWith("/access/scopes")) return route.fulfill({ json: { token: "scope-fixture", expires: Date.now() / 1000 + 300 } });

    const path = new URL(route.request().url()).pathname;
    if (path.includes("/account/security")) return route.fallback();
    if (path.endsWith("/data/providers"))
      return route.fulfill({ json: [providerFixture(damaged)] });
    if (path.endsWith("/configuration")) {
      if (route.request().method() === "POST") {
        const body = route.request().postDataJSON();
        submitted = body;
        state = {
          ...state,
          revision: 4,
          values: body.values,
          secret_fields: ["access_key"],
          configured: true,
          error: null,
        };
      }
      return route.fulfill({ json: state });
    }
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
  await login(page, "/?screen=settings");
  await page.getByRole("button", { name: "数据源", exact: true }).click();
  const card = page.getByRole("region", { name: damaged.name, exact: true });
  await expect(card.getByRole("alert")).toHaveText(
    "配置快照损坏，请重新填写完整配置",
  );
  await expect(card.getByText("待配置", { exact: true })).toBeVisible();
  await expect(
    card.getByRole("button", { name: "测试当前配置", exact: true }),
  ).toBeDisabled();
  await card.getByLabel("连接名称", { exact: true }).fill("恢复后的连接");
  await card.getByLabel("启用压缩", { exact: true }).check();
  const key = card.getByLabel("访问密钥", { exact: true });
  await key.fill("replacement-synthetic-secret");
  await card.getByRole("button", { name: "保存配置", exact: true }).click();
  await expect(card.getByRole("status")).toContainText("配置已保存");
  expect(submitted).toEqual({
    expected_revision: 3,
    values: { label: "恢复后的连接", enabled: true },
    secrets: { access_key: "replacement-synthetic-secret" },
  });
  await expect(key).toHaveValue("");
  await expect(card.getByRole("alert")).toHaveCount(0);
  await expect(card.getByText("配置就绪", { exact: true })).toBeVisible();
  await expect(
    card.getByRole("button", { name: "测试当前配置", exact: true }),
  ).toBeEnabled();
  await card
    .getByRole("button", { name: "刷新已保存配置", exact: true })
    .click();
  await expect(card.getByLabel("连接名称", { exact: true })).toHaveValue(
    "恢复后的连接",
  );
  await expect(card.getByRole("alert")).toHaveCount(0);
});
