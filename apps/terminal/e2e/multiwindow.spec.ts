import { expect, test } from "@playwright/test";
import { nativeContext } from "./native";
test("native windows share login and logout but keep independent layouts", async ({
  context,
  page,
}) => {
  const host = await nativeContext(context);
  const user = {
    email: "test@example.com",
    first_name: "Test",
    last_name: "User",
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
  const second = await context.newPage();
  await second.goto("/?view=研究&window=workspace-test");
  await expect(
    second.getByRole("navigation", { name: "业务工作区" }),
  ).toBeVisible();
  await expect(
    second.getByRole("heading", { name: "研究", exact: true }),
  ).toBeVisible();
  await page
    .getByRole("navigation", { name: "业务工作区" })
    .getByRole("button", { name: "数据", exact: true })
    .click();
  await second.getByRole("button", { name: "任务", exact: true }).click();
  await expect.poll(() => host.layout("workspace-test")?.tasks).toBe(true);
  const state = { main: host.layout()!, second: host.layout("workspace-test")! };
  expect(state.main.view).toBe("数据");
  expect(state.second.view).toBe("研究");
  expect(state.main.tasks).toBe(false);
  expect(state.second.tasks).toBe(true);
  await second.getByRole("button", { name: "退出账号" }).click();
  await expect(
    page.getByRole("heading", { name: "登录", exact: true }),
  ).toBeVisible();
  await expect(
    second.getByRole("heading", { name: "登录", exact: true }),
  ).toBeVisible();
});

test("chart detaches, merges and restores resized layout", async ({
  context,
  page,
}) => {
  await nativeContext(context);
  const user = {
    email: "test@example.com",
    first_name: "Test",
    last_name: "User",
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
  const divider = page.getByRole("separator", { name: "行情与图表分隔线" });
  await divider.focus();
  await divider.press("ArrowLeft");
  await expect(divider).toHaveAttribute("aria-valuenow", "58");
  const childPromise = context.waitForEvent("page");
  await page.getByRole("button", { name: "拆出图表", exact: true }).click();
  const child = await childPromise;
  await expect(
    child.getByRole("button", { name: "归并图表", exact: true }),
  ).toBeVisible();
  await expect(page.getByText("图表已在独立窗口打开")).toBeVisible();
  await page.getByRole("button", { name: "归并图表", exact: true }).click();
  await expect(
    page.getByRole("button", { name: "拆出图表", exact: true }),
  ).toBeVisible();
  await page.reload();
  await expect(divider).toHaveAttribute("aria-valuenow", "58");
});
