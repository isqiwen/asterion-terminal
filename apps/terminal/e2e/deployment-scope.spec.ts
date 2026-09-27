import { openSettingsWindow, closeSettingsWindow } from "./settings-helper";
import { test, expect } from "@playwright/test";

test("local deployment needs no SSH and remote deployment is Linux only", async ({ page }) => {
  await page.goto("/");
  page = await openSettingsWindow(page);
  await page.getByRole("button", { name: "连接与部署", exact: true }).click();
  await expect(page.getByRole("button", { name: "本机部署", exact: true })).toHaveAttribute("aria-pressed", "true");
  await expect(page.getByLabel("本机部署说明")).toContainText("无需 SSH、密钥或机器初始化脚本");
  await expect(page.getByLabel("SSH 私钥", { exact: true })).toHaveCount(0);
  await expect(page.getByText("通过 SSH 添加机器", { exact: true })).toHaveCount(0);
  await page.getByRole("button", { name: "远程 Linux", exact: true }).click();
  await expect(page.getByLabel("本机部署说明")).toHaveCount(0);
  await page.getByText("通过 SSH 添加机器", { exact: true }).click();
  await expect(page.getByLabel("SSH 用户", { exact: true })).toHaveValue("asterion");
  await page.getByText("部署服务", { exact: true }).click();
  await expect(page.getByLabel("程序目标系统")).toHaveCount(0);
  await expect(page.getByLabel("目标平台 Agent 程序")).toHaveCount(0);
  await expect(page.getByLabel("程序文件绝对路径")).toHaveCount(0);
  await page.getByRole("button", { name: "本机部署", exact: true }).click();
  await expect(page.getByLabel("SSH 私钥", { exact: true })).toHaveCount(0);
});

test("Terminal generates and reuses a public key without exposing the private key", async ({ page, context }) => {
  await page.goto("/");
  page = await openSettingsWindow(page);
  await page.getByRole("button", { name: "连接与部署", exact: true }).click();
  await page.getByRole("button", { name: "远程 Linux", exact: true }).click();
  await page.getByText("通过 SSH 添加机器", { exact: true }).click();
  await page.getByLabel("机器名称", { exact: true }).fill("managed-ui");
  await expect(page.getByLabel("SSH 密钥来源", { exact: true })).toHaveValue("managed");
  await expect(page.getByLabel("SSH 私钥", { exact: true })).toHaveCount(0);
  await page.getByRole("button", { name: "生成或查看本机公钥", exact: true }).click();
  const publicKey = page.getByLabel("初始化公钥", { exact: true });
  await expect(publicKey).toHaveValue(/^ssh-ed25519 /);
  const original = await publicKey.inputValue();
  await context.grantPermissions(["clipboard-read", "clipboard-write"]);
  await page.getByRole("button", { name: "复制公钥", exact: true }).click();
  await expect(page.getByRole("status").filter({ hasText: "公钥已复制" })).toBeVisible();
  expect(await page.evaluate(() => navigator.clipboard.readText())).toBe(original);
  await page.getByRole("button", { name: "生成或查看本机公钥", exact: true }).click();
  await expect(publicKey).toHaveValue(original);
  expect(await page.evaluate(() => JSON.stringify(localStorage))).not.toContain("PRIVATE KEY");
  await expect(page.locator("body")).not.toContainText("BEGIN OPENSSH PRIVATE KEY");
});


test("the Linux initializer is exported from the bundled resources", async ({ page }) => {
  await page.goto("/");
  page = await openSettingsWindow(page);
  await page.getByRole("button", { name: "连接与部署", exact: true }).click();
  await page.getByRole("button", { name: "远程 Linux", exact: true }).click();
  const download = page.waitForEvent("download");
  await page.getByRole("button", { name: "导出 Linux 初始化脚本", exact: true }).click();
  const file = await download;
  expect(file.suggestedFilename()).toBe("initialize-linux.py");
  const stream = await file.createReadStream();
  let content = "";
  for await (const chunk of stream!) content += chunk.toString();
  expect(content).toContain("ACCOUNT = 'asterion'");
  expect(content).toContain("--public-key");
});
