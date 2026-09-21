import { test, expect } from "@playwright/test";
import { nativeContext } from "./native";

test("backup verification and explicit isolated restore with error recovery", async ({context,page}) => {
  let failure = true;
  let restores = 0;
  let active = "/original";
  let previous: string | null = null;
  let activations = 0;
  await nativeContext(context, {
    desktop_environment: () => ({active,previous,pending:null,protection_backup:previous ? "/backups/protection.zip" : null}),
    desktop_activate: args => {
      activations++;
      const outgoing = active;
      active = typeof args.target === "string" ? args.target : previous!;
      previous = outgoing;
      return {status:"active"};
    },
    desktop_backup: async () => {
      if (failure) throw new Error("备份验证失败，原数据保留");
      return {status:"verified",path:"/backups/test.zip",bytes:100,sha256:"verified-digest",verification:{versions:5,research_results:2,quarantined_tasks:1}};
    },
    desktop_restore: async args => {
      expect(args.archive).toBe("/backups/test.zip");
      restores++;
      if (args.target === "/existing") throw new Error("恢复目标已存在，不覆盖数据");
      expect(args.target).toBe("/new-recovery");
      return {status:"verified",target:args.target,versions:5,research_results:2,quarantined_tasks:1};
    },
  });
  await context.route("**/api/v1/**",route => {
    if (route.request().url().endsWith("/access/scopes")) return route.fulfill({ json: { token: "scope-fixture", expires: Date.now() / 1000 + 300 } });

    const path = new URL(route.request().url()).pathname;
    if (path.includes("/security")) return route.fallback();
    const user = {email:"backup@example.com",first_name:"Backup",last_name:"Test"};
    return route.fulfill({json:path.endsWith("/login") ? {session:"session",user} : path.endsWith("/me") ? user : path.endsWith("/health") ? {status:"ready"} : []});
  });
  await page.goto("/?screen=settings");
  await page.getByLabel("邮箱",{exact:true}).fill("backup@example.com");
  await page.getByLabel("密码",{exact:true}).fill("fixture-password");
  await page.getByRole("button",{name:"登录",exact:true}).click();
  await page.getByRole("button",{name:"备份与恢复",exact:true}).click();
  await expect(page.getByRole("button",{name:"验证并恢复到新目录",exact:true})).toBeDisabled();
  await page.getByRole("button",{name:"创建备份并验证",exact:true}).click();
  await expect(page.getByRole("alert")).toContainText("原数据保留");
  failure = false;
  await page.getByRole("button",{name:"创建备份并验证",exact:true}).click();
  await expect(page.getByText(/备份已验证/)).toBeVisible();
  expect(restores).toBe(0);
  await expect(page.getByLabel("备份文件完整路径")).toHaveValue("/backups/test.zip");
  await page.getByLabel("新恢复目录完整路径").fill("/existing");
  await page.getByRole("button",{name:"验证并恢复到新目录",exact:true}).click();
  await expect(page.getByRole("alert")).toContainText("不覆盖数据");
  await page.getByLabel("新恢复目录完整路径").fill("/new-recovery");
  await page.getByRole("button",{name:"验证并恢复到新目录",exact:true}).click();
  await expect(page.getByText(/恢复验证通过/)).toBeVisible();
  await expect(page.getByText("当前工作台未切换。恢复目录中的后台未启动。",{exact:true})).toBeVisible();
  expect(activations).toBe(0);
  await expect(page.getByLabel("已恢复目录完整路径")).toHaveValue("/new-recovery");
  await page.getByRole("button",{name:"保护备份并切换",exact:true}).click();
  await expect(page.getByText("当前环境：")).toContainText("/new-recovery");
  await expect(page.getByText("最近保护备份：")).toContainText("/backups/protection.zip");
  await page.getByRole("button",{name:"保护备份并回滚",exact:true}).click();
  await expect(page.getByText("当前环境：")).toContainText("/original");
  expect(activations).toBe(2);
});
