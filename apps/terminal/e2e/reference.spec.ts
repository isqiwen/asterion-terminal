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
  let publication: Record<string, unknown> | null = null;
  await context.route("**/api/v1/**", (route) => {
    const path = new URL(route.request().url()).pathname;
    if (path.includes("/account/security")) return route.fallback();
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
    page.getByText("尚无合约资料。", { exact: false }),
  ).toBeVisible();
  await page.getByRole("button", { name: "规则版本", exact: true }).click();
  await expect(
    page.getByText("尚无合约规则版本", { exact: false }),
  ).toBeVisible();
  await page.getByLabel("导入合约资料").setInputFiles({
    name: "catalog.json",
    mimeType: "application/json",
    buffer: Buffer.from(
      JSON.stringify({
        products: [],
        contracts: [],
        calendars: [],
        rules: [],
      }),
    ),
  });
  await expect(page.getByRole("heading", { name: "合约明细" })).toBeVisible();
  await expect(page.getByText("synthetic-test", { exact: true })).toBeVisible();
  await page.getByLabel("导入合约资料").setInputFiles({
    name: "invalid.json",
    mimeType: "application/json",
    buffer: Buffer.from("{}"),
  });
  await expect(page.getByRole("alert")).toContainText("目录缺少品种字段");
  await expect(page.getByRole("heading", { name: "合约明细" })).toBeVisible();
});
