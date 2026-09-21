import { expect, test } from "@playwright/test";
import { nativeContext } from "./native";
async function routeAccount(
  context: import("@playwright/test").BrowserContext,
) {
  const user = {
    email: "pin@example.com",
    first_name: "Pin",
    last_name: "Test",
  };
  await context.route("**/api/v1/**", (route) => {
    if (route.request().url().endsWith("/access/scopes")) return route.fulfill({ json: { token: "scope-fixture", expires: Date.now() / 1000 + 300 } });

    const path = new URL(route.request().url()).pathname;
    if (path.includes("/account/security")) return route.fallback();
    if (path.endsWith("/data/catalog")) return route.fulfill({json:{items:[],total:0}});
    if (path.endsWith("/research/workspace")) return route.fulfill({json:{draft:null,templates:[]}});
    if (path.endsWith("/research/workspace/draft")) return route.fulfill({json:{id:"draft",...route.request().postDataJSON(),revision:route.request().postDataJSON().expected_revision+1}});

    return route.fulfill({
      json: path.endsWith("/login")
        ? { session: "test-session", user }
        : path.endsWith("/me")
          ? user
          : path.endsWith("/health")
            ? { status: "ready" }
            : [],
    });
  });
}
async function login(page: import("@playwright/test").Page) {
  await page.goto("/");
  await page.getByLabel("邮箱", { exact: true }).fill("pin@example.com");
  await page.getByLabel("密码", { exact: true }).fill("test-password-123");
  await page.getByRole("button", { name: "登录", exact: true }).click();
}
test("idle locks workspaces and settings together; incorrect PIN stays locked", async ({
  page,
  context,
}) => {
  const security = await nativeContext(context);
  await routeAccount(context);
  await login(page);
  await expect(
    page.getByRole("navigation", { name: "业务工作区" }),
  ).toBeVisible();
  await page
    .getByRole("navigation", { name: "业务工作区" })
    .getByRole("button", { name: "研究", exact: true })
    .click();
  const settings = await context.newPage();
  await settings.goto("/?screen=settings&window=settings");
  await settings.getByRole("button", { name: "安全", exact: true }).click();
  const saved = settings.waitForResponse(
    (r) =>
      r.url().endsWith("/security/timeout") && r.request().method() === "POST",
  );
  await settings.getByLabel("无操作后自动锁定").selectOption("60");
  await saved;
  security.advance(61);
  await expect(
    page.getByRole("heading", { name: "终端已锁定", exact: true }),
  ).toBeVisible();
  await expect(
    settings.getByRole("heading", { name: "终端已锁定", exact: true }),
  ).toBeVisible();
  await expect(
    page.getByRole("navigation", { name: "业务工作区" }),
  ).toBeHidden();
  await page.getByLabel("PIN", { exact: true }).fill("000000");
  await page.getByRole("button", { name: "解锁", exact: true }).click();
  await expect(page.getByRole("alert")).toContainText("PIN 不正确");
  await page.screenshot({ path: "../../.state/pin-lock.png" });
  await page.getByLabel("PIN", { exact: true }).fill("246810");
  await page.getByRole("button", { name: "解锁", exact: true }).click();
  await expect(
    page.getByRole("heading", { name: "研究", exact: true }),
  ).toBeVisible();
  await expect(
    settings.getByRole("heading", { name: "安全", exact: true }),
  ).toBeVisible();
  await page.getByRole("button", { name: "锁定终端", exact: true }).click();
  await expect(
    page.getByRole("heading", { name: "终端已锁定", exact: true }),
  ).toBeVisible();
});
test("service reconnection keeps a locked workspace protected", async ({
  page,
  context,
}) => {
  await nativeContext(context);
  await routeAccount(context);
  let offline = false;
  await context.route("**/api/v1/account/security", (route) =>
    offline
      ? route.fulfill({ status: 503, json: { detail: "服务暂不可用" } })
      : route.fallback(),
  );
  await login(page);
  await page.getByRole("button", { name: "锁定终端", exact: true }).click();
  await expect(
    page.getByRole("heading", { name: "终端已锁定", exact: true }),
  ).toBeVisible();
  offline = true;
  await expect(
    page.getByRole("button", { name: "重连本机服务" }),
  ).toBeVisible();
  await page.getByRole("button", { name: "重连本机服务" }).click();
  offline = false;
  await expect(
    page.getByRole("heading", { name: "终端已锁定", exact: true }),
  ).toBeVisible();
  await expect(
    page.getByRole("navigation", { name: "业务工作区" }),
  ).toBeHidden();
  await page.getByLabel("PIN", { exact: true }).fill("246810");
  await page.getByRole("button", { name: "解锁", exact: true }).click();
  await expect(
    page.getByRole("navigation", { name: "业务工作区" }),
  ).toBeVisible();
});
