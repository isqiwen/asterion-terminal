import { expect, test } from "@playwright/test";
import { nativeContext } from "./native";
test("registration, verification, login and logout follow service responses", async ({
  page,
}) => {
  await nativeContext(page.context());
  let verified = false;
  let registerCalls = 0;
  await page.route("**/api/v1/**", async (route) => {
    const path = new URL(route.request().url()).pathname;
    if (path.includes("/account/security")) return route.fallback();
    const body = route.request().postDataJSON();
    if (path.endsWith("/register")) {
      registerCalls++;
      expect(body.first_name).toBe("Test");
      await route.fulfill({ json: { status: "verification_sent" } });
    } else if (path.endsWith("/verify")) {
      verified = body.code === "123456";
      await route.fulfill({
        status: verified ? 200 : 400,
        json: verified
          ? { status: "verified" }
          : { detail: "验证码无效", code: "INVALID_CODE" },
      });
    } else if (path.endsWith("/login")) {
      expect(verified).toBe(true);
      await route.fulfill({
        json: {
          session: "test-session",
          user: {
            email: "test@example.com",
            first_name: "Test",
            last_name: "User",
          },
        },
      });
    } else if (path.endsWith("/me"))
      await route.fulfill({
        json: {
          email: "test@example.com",
          first_name: "Test",
          last_name: "User",
        },
      });
    else if (path.endsWith("/health"))
      await route.fulfill({ json: { status: "ready" } });
    else {
      if (path.endsWith("/snapshots"))
        expect(route.request().headers()["x-account-session"]).toBe(
          "test-session",
        );
      await route.fulfill({ json: [] });
    }
  });
  await page.goto("/");
  await expect(
    page.getByRole("heading", { name: "登录", exact: true }),
  ).toBeVisible();
  await page.screenshot({ path: "../../.state/account-login.png" });
  await page.getByRole("button", { name: "注册", exact: true }).click();
  await page.getByLabel("名", { exact: true }).fill("Test");
  await page.getByLabel("姓", { exact: true }).fill("User");
  await page.getByLabel("邮箱", { exact: true }).fill("test@example.com");
  await page.getByLabel("密码", { exact: true }).fill("correct-password-123");
  await page
    .getByLabel("确认密码", { exact: true })
    .fill("different-password-123");
  await page.getByLabel("终端 PIN（6 位数字）").fill("246810");
  await page.getByLabel("确认 PIN", { exact: true }).fill("246810");
  await page.getByRole("button", { name: "创建账号", exact: true }).click();
  await expect(page.getByRole("alert")).toContainText("不一致");
  expect(registerCalls).toBe(0);
  await page
    .getByLabel("确认密码", { exact: true })
    .fill("correct-password-123");
  await page.screenshot({ path: "../../.state/account-register.png" });
  await page.getByRole("button", { name: "创建账号", exact: true }).click();
  await expect(page.getByRole("heading", { name: "验证邮箱" })).toBeVisible();
  await expect(
    page.getByRole("button", { name: /重新发送验证码/ }),
  ).toBeDisabled();
  await page.screenshot({ path: "../../.state/account-verify.png" });
  await page.getByLabel("6 位验证码").fill("999999");
  await page.getByRole("button", { name: "验证邮箱", exact: true }).click();
  await expect(page.getByRole("alert")).toContainText("验证码无效");
  await page.getByLabel("6 位验证码").fill("123456");
  await page.getByRole("button", { name: "验证邮箱", exact: true }).click();
  await expect(page.getByRole("status")).toContainText("验证成功");
  await page.getByLabel("密码", { exact: true }).fill("correct-password-123");
  await page.getByRole("button", { name: "登录", exact: true }).click();
  await expect(
    page.getByRole("navigation", { name: "业务工作区" }),
  ).toBeVisible();
  const storage = await page.evaluate(() => JSON.stringify(localStorage));
  expect(storage).not.toContain("correct-password");
  expect(storage).not.toContain("test-session");
  await page.getByRole("button", { name: "退出账号" }).click();
  await expect(
    page.getByRole("heading", { name: "登录", exact: true }),
  ).toBeVisible();
  await expect(page.getByLabel("密码", { exact: true })).toBeEmpty();
});

test("password reset waits for email code and matching passwords", async ({
  page,
}) => {
  await nativeContext(page.context());
  let resets = 0;
  await page.route("**/api/v1/**", async (route) => {
    if (route.request().url().endsWith("/reset")) {
      resets++;
      expect(route.request().postDataJSON().code).toBe("123456");
    }
    await route.fulfill({ json: { status: "ready" } });
  });
  await page.goto("/");
  await page.getByRole("button", { name: "忘记密码？" }).click();
  await page.getByLabel("邮箱", { exact: true }).fill("test@example.com");
  await page.getByRole("button", { name: "发送重置邮件" }).click();
  await expect(page.getByLabel("6 位验证码")).toBeVisible();
  await page.getByLabel("密码", { exact: true }).fill("new-password-123");
  await page.getByLabel("确认密码").fill("new-password-124");
  await page.getByLabel("6 位验证码").fill("123456");
  await page.getByRole("button", { name: "找回密码", exact: true }).click();
  await expect(page.getByRole("alert")).toContainText("不一致");
  expect(resets).toBe(0);
  await page.getByLabel("确认密码").fill("new-password-123");
  await page.getByRole("button", { name: "找回密码", exact: true }).click();
  await expect(page.getByRole("status")).toContainText("密码已更新");
  expect(resets).toBe(1);
});

test("local entry explains verification and offers no mail configuration", async ({
  page,
}) => {
  await nativeContext(page.context());
  await page.route("**/api/v1/**", async (route) => {
    await route.fulfill({
      json: route.request().url().endsWith("/capabilities")
        ? { verification: "local", code_length: 6 }
        : { status: "ready" },
    });
  });
  await page.goto("/");
  await expect(page.getByText("本地账号 · 无需邮件服务")).toBeVisible();
  await expect(page.getByRole("button", { name: "配置邮件服务" })).toHaveCount(
    0,
  );
  await page.getByRole("button", { name: "已有验证码？验证邮箱" }).click();
  await expect(
    page.getByText("本地模式：输入任意 6 位数字即可完成验证，无需收取邮件。"),
  ).toBeVisible();
  await page.getByLabel("邮箱", { exact: true }).fill("local@example.com");
  await page.getByLabel("6 位验证码").fill("000000");
  await page.getByRole("button", { name: "验证邮箱", exact: true }).click();
  await expect(page.getByRole("status")).toContainText("本地账号已确认");
});
