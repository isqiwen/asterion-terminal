import { expect, test } from "@playwright/test";
import { nativeContext } from "./native";
test("reference directory imports a version and shows publication failures", async ({
  context,
  page,
}) => {
  await nativeContext(context);
  const user = {
    email: "catalog@example.com",
    first_name: "Catalog",
    last_name: "Test",
  };
  let sourcePublications = 0;
  let publication: Record<string, unknown> | null = null;
  await context.route("**/api/v1/**", (route) => {
    if (route.request().url().endsWith("/access/scopes"))
      return route.fulfill({
        json: { token: "scope-fixture", expires: Date.now() / 1000 + 300 },
      });

    const path = new URL(route.request().url()).pathname;
    if (path.includes("/account/security")) return route.fallback();
    if (path.endsWith("/data/catalog"))
      return route.fulfill({ json: { items: [], total: 0 } });
    if (path.endsWith("/research/workspace"))
      return route.fulfill({ json: { draft: null, templates: [] } });
    if (path.endsWith("/research/workspace/draft"))
      return route.fulfill({
        json: {
          id: "draft",
          ...route.request().postDataJSON(),
          revision: route.request().postDataJSON().expected_revision + 1,
        },
      });

    if (path.includes("/reference/source/")) {
      const request = route.request().postDataJSON();
      if (
        request.version_id !== "fixed-source" ||
        request.symbols.join(",") !== "RB2610.SHF"
      )
        return route.fulfill({
          status: 422,
          json: { detail: "所选来源代码不在固定合约资料版本中" },
        });
      const catalog = {
        ...(publication?.catalog as object),
        inputs: [
          {
            version_id: "fixed-source",
            checksum: "a".repeat(64),
            source: "tushare",
          },
        ],
      };
      if (path.endsWith("/preview")) return route.fulfill({ json: catalog });
      sourcePublications++;
      publication = { id: "source-release", published_at: 1789600000, catalog };
      return route.fulfill({ json: publication });
    }
    if (path.endsWith("/reference/releases")) {
      if (route.request().method() === "POST") {
        const catalog = route.request().postDataJSON();
        if (!catalog.products)
          return route.fulfill({
            status: 422,
            json: { detail: "目录缺少品种字段" },
          });
        publication = {
          id: "test-version-001",
          published_at: 1789600000,
          catalog,
        };
        return route.fulfill({ json: publication });
      }
      return route.fulfill({
        json: publication
          ? [
              {
                id: publication.id,
                published_at: publication.published_at,
                sources: ["synthetic-test"],
                products: 0,
                contracts: 0,
              },
            ]
          : [],
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
  await page.goto("/");
  await page.getByLabel("邮箱", { exact: true }).fill(user.email);
  await page.getByLabel("密码", { exact: true }).fill("password-test-123");
  await page.getByRole("button", { name: "登录", exact: true }).click();
  await page
    .getByRole("navigation", { name: "业务工作区" })
    .getByRole("button", { name: "数据", exact: true })
    .click();
  await page.getByRole("button", { name: "合约资料", exact: true }).click();
  await expect(
    page.getByText("没有符合条件的数据集。", { exact: false }),
  ).toBeVisible();
  await page.getByRole("button", { name: "合约目录", exact: true }).click();
  await expect(
    page.getByText("尚无合约目录版本", { exact: false }),
  ).toBeVisible();
  await page.getByLabel("导入合约资料").setInputFiles({
    name: "catalog.json",
    mimeType: "application/json",
    buffer: Buffer.from(
      JSON.stringify({
        products: [
          {
            id: "SHFE.RB",
            exchange: "SHFE",
            name: "test",
            currency: "CNY",
            provenance: {
              source: "synthetic-test",
              source_version: "1",
              observed_at: "2026-01-01T00:00:00Z",
              available_at: "2026-01-01T00:01:00Z",
            },
          },
        ],
        contracts: [
          {
            id: "SHFE.RB.202610.20251001",
            product_id: "SHFE.RB",
            delivery_month: "2026-10",
            listed_on: "2025-10-01",
            last_trade_on: "2026-10-15",
            last_delivery_on: null,
            provenance: {
              source: "synthetic-test",
              source_version: "1",
              observed_at: "2026-01-01T00:00:00Z",
              available_at: "2026-01-01T00:01:00Z",
            },
          },
        ],
        schema_version: 2,
        inputs: [],
        symbols: [
          {
            source: "tushare",
            symbol: "RB2610.SHF",
            contract_id: "SHFE.RB.202610.20251001",
            valid_from: "2025-10-01",
            valid_until: "2026-10-15",
            provenance: {
              source: "synthetic-test",
              source_version: "1",
              observed_at: "2026-01-01T00:00:00Z",
              available_at: "2026-01-01T00:01:00Z",
            },
          },
        ],
      }),
    ),
  });
  await expect(page.getByRole("heading", { name: "合约明细" })).toBeVisible();
  await expect(page.getByText("synthetic-test", { exact: true })).toBeVisible();
  await expect(
    page.getByRole("heading", { name: "来源代码映射" }),
  ).toBeVisible();
  await expect(
    page.getByRole("cell", { name: "RB2610.SHF", exact: true }),
  ).toBeVisible();
  await expect(
    page.getByRole("cell", { name: "SHFE.RB.202610.20251001", exact: true }),
  ).toHaveCount(2);
  await page.getByLabel("导入合约资料").setInputFiles({
    name: "invalid.json",
    mimeType: "application/json",
    buffer: Buffer.from("{}"),
  });
  await expect(page.getByRole("alert")).toContainText("目录缺少品种字段");
  await expect(page.getByRole("heading", { name: "合约明细" })).toBeVisible();
  await page.getByLabel("固定资料版本 ID").fill("fixed-source");
  await page.getByLabel("来源合约代码").fill("missing");
  await page.getByRole("button", { name: "预览身份目录" }).click();
  await expect(
    page.getByText("所选来源代码不在固定合约资料版本中", { exact: false }),
  ).toBeVisible();
  await expect(page.getByRole("button", { name: "发布身份目录" })).toHaveCount(
    0,
  );
  await page.getByLabel("来源合约代码").fill("RB2610.SHF");
  await page.getByRole("button", { name: "预览身份目录" }).click();
  await expect(page.getByLabel("身份目录预览")).toContainText(
    "SHFE.RB.202610.20251001",
  );
  expect(sourcePublications).toBe(0);
  await page.getByLabel("固定资料版本 ID").fill("other-version");
  await expect(page.getByRole("button", { name: "发布身份目录" })).toHaveCount(
    0,
  );
  await page.getByLabel("固定资料版本 ID").fill("fixed-source");
  await page.getByRole("button", { name: "预览身份目录" }).click();
  await page.getByRole("button", { name: "发布身份目录" }).click();
  await expect(page.getByLabel("目录输入证据")).toContainText("fixed-source");
  expect(sourcePublications).toBe(1);
});
