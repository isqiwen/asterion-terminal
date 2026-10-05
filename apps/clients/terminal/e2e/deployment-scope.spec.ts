import { openSettingsWindow } from "./settings-helper";
import { test, expect } from "./test";

test("default deployment shows active locations without SSH or certificate forms", async ({
  page,
}) => {
  await page.goto("/");
  page = await openSettingsWindow(page);
  await page.getByRole("button", { name: "连接与部署", exact: true }).click();
  const locations = page.getByRole("list", { name: "当前运行位置" });
  await expect(locations).toBeVisible();
  for (const service of ["实时行情", "历史数据服务", "任务服务"]) {
    await expect(locations.getByRole("listitem").filter({ hasText: service })).toContainText(
      "本机",
    );
  }
  await expect(page.getByLabel("SSH 地址", { exact: true })).toHaveCount(0);
  await expect(page.getByLabel("客户端证书文件", { exact: true })).not.toBeVisible();
  await page.screenshot({ path: "apps/clients/terminal/test-results/deployment-overview.png" });
  await page.getByRole("button", { name: "添加远程机器", exact: true }).click();
  await expect(page.getByRole("region", { name: "添加远程机器" })).toContainText("Linux x86_64");
  await expect(page.getByLabel("已核验 known_hosts 文件", { exact: true })).toHaveCount(0);
  await page
    .getByRole("region", { name: "添加远程机器" })
    .getByRole("button", { name: "取消", exact: true })
    .click();
  await page.getByRole("button", { name: "当前运行位置", exact: true }).click();
  await expect(locations).toBeVisible();
  await locations.getByRole("button", { name: "管理", exact: true }).first().click();
  await expect(page.getByText(/^节点准入预算：CPU/)).toContainText(/内存 \d+\/\d+ MiB/);
});

test("machine wizard generates a reusable public key and requires initialization and identity checks", async ({
  page,
  context,
}) => {
  await page.goto("/");
  page = await openSettingsWindow(page);
  await page.getByRole("button", { name: "连接与部署", exact: true }).click();
  await page.getByRole("button", { name: "添加远程机器", exact: true }).click();
  const wizard = page.getByRole("region", { name: "添加远程机器" });
  await wizard.getByLabel("机器名称", { exact: true }).fill("managed-ui");
  await wizard.getByLabel("SSH 地址", { exact: true }).fill("192.0.2.10");
  await wizard.getByRole("button", { name: "下一步", exact: true }).click();
  const publicKey = wizard.getByLabel("初始化公钥", { exact: true });
  await expect(publicKey).toHaveValue(/^ssh-ed25519 /);
  const original = await publicKey.inputValue();
  await context.grantPermissions(["clipboard-read", "clipboard-write"]);
  await wizard.getByRole("button", { name: "复制公钥", exact: true }).click();
  await expect(wizard.getByRole("status")).toContainText("公钥已复制");
  expect((await page.evaluate(() => navigator.clipboard.readText())).trim()).toBe(original.trim());
  const download = page.waitForEvent("download");
  await wizard.getByRole("button", { name: "导出 Linux 初始化脚本" }).click();
  const file = await download;
  expect(file.suggestedFilename()).toBe("initialize-linux.py");
  const stream = await file.createReadStream();
  let content = "";
  for await (const chunk of stream!) content += chunk.toString();
  expect(content).toContain("ACCOUNT = 'asterion'");
  expect(content).toContain("--public-key");
  await expect(wizard.getByRole("button", { name: "下一步", exact: true })).toBeDisabled();
  await page.screenshot({ path: "apps/clients/terminal/test-results/deployment-initialize.png" });
  await wizard.getByRole("button", { name: "上一步", exact: true }).click();
  await wizard.getByRole("button", { name: "下一步", exact: true }).click();
  await expect(publicKey).toHaveValue(original);
  await wizard.getByLabel("管理员已完成初始化").check();
  await wizard.getByRole("button", { name: "下一步", exact: true }).click();
  await expect(wizard.getByRole("button", { name: "安装并连接", exact: true })).toBeDisabled();
  await expect(wizard.getByLabel("已核验 known_hosts 文件", { exact: true })).toBeVisible();
  await wizard.getByLabel("已核验 known_hosts 文件", { exact: true }).fill("/tmp/verified-hosts");
  await wizard.getByRole("checkbox", { name: "已通过可信渠道核对主机指纹" }).check();
  await expect(wizard.getByRole("button", { name: "安装并连接", exact: true })).toBeEnabled();
  await wizard.getByRole("button", { name: "上一步", exact: true }).click();
  await wizard.getByRole("button", { name: "上一步", exact: true }).click();
  await wizard.getByLabel("SSH 地址", { exact: true }).fill("192.0.2.11");
  await wizard.getByRole("button", { name: "下一步", exact: true }).click();
  await expect(wizard.getByLabel("管理员已完成初始化")).not.toBeChecked();
  await wizard.getByLabel("管理员已完成初始化").check();
  await wizard.getByRole("button", { name: "下一步", exact: true }).click();
  await expect(
    wizard.getByRole("checkbox", { name: "已通过可信渠道核对主机指纹" }),
  ).not.toBeChecked();
  await expect(wizard.getByRole("button", { name: "安装并连接", exact: true })).toBeDisabled();
  expect(await page.evaluate(() => JSON.stringify(localStorage))).not.toContain("PRIVATE KEY");
  await expect(wizard).not.toContainText("BEGIN OPENSSH PRIVATE KEY");
});
