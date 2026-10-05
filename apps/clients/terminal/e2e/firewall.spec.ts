import { removeFolder } from "./cleanup";
import { test, expect } from "./test";
import { openSettingsWindow } from "./settings-helper";
import { mkdtemp, writeFile, readFile } from "node:fs/promises";
import { execFileSync } from "node:child_process";
import { tmpdir } from "node:os";
import { join } from "node:path";
test("firewall changes require a concrete preview and explicit confirmation", async ({
  page: workbench,
}) => {
  const folder = await mkdtemp(join(tmpdir(), "asterion-fw-ui-"));
  try {
    const artifact = Buffer.alloc(64);
    Buffer.from([0x7f, 69, 76, 70, 2, 1]).copy(artifact);
    artifact[18] = 62;
    await writeFile(join(folder, "linux-inspection-only"), artifact);
    await writeFile(join(folder, "known_hosts"), "isolated SSH test double only\n");
    execFileSync("/usr/bin/ssh-keygen", [
      "-q",
      "-t",
      "ed25519",
      "-N",
      "",
      "-f",
      join(folder, "key"),
    ]);
    const key = await readFile(join(folder, "key"), "utf8");
    const changes = join(process.env.ASTERION_SSH_FIXTURE!, "firewall-changes");
    const before = await readFile(changes, "utf8").catch(() => "");
    await workbench.goto("/");
    // Settings is its own window; every step below runs inside it.
    const page = await openSettingsWindow(workbench);
    await page.getByRole("button", { name: "连接与部署", exact: true }).click();
    await page.getByRole("button", { name: "添加远程机器", exact: true }).click();
    await page.getByText("高级连接选项", { exact: true }).click();
    for (const [label, value] of [
      ["机器名称", "fw-ui"],
      ["SSH 地址", "localhost"],
      ["SSH 用户", "tester"],
    ])
      await page.getByLabel(label, { exact: true }).fill(value);
    await page.getByLabel("SSH 密钥来源", { exact: true }).selectOption("provided");
    await page.getByRole("button", { name: "下一步", exact: true }).click();
    await page.getByLabel("管理员已完成初始化").check();
    await page.getByRole("button", { name: "下一步", exact: true }).click();
    await page
      .getByLabel("已核验 known_hosts 文件", { exact: true })
      .fill(join(folder, "known_hosts"));
    await page.getByLabel("已通过可信渠道核对主机指纹").check();
    await page.getByLabel("SSH 私钥", { exact: true }).fill(key);
    await page.getByText("安装前检查防火墙", { exact: true }).click();
    await page.getByRole("button", { name: "检查并预览放行规则", exact: true }).click();
    const preview = page.getByRole("region", { name: "防火墙规则预览" });
    await expect(preview).toContainText("192.0.2.10");
    await expect(preview).toContainText("7442");
    expect(await readFile(changes, "utf8").catch(() => "")).toBe(before);
    await preview.getByRole("button", { name: "确认放行上述来源和端口" }).click();
    await expect(preview).toContainText("请安装并连接服务管理器");
    await expect(page.getByLabel("SSH 私钥", { exact: true })).toHaveValue("");
    expect(await page.evaluate(() => JSON.stringify(localStorage))).not.toContain("PRIVATE KEY");
    expect(await readFile(changes, "utf8")).not.toBe(before);
    await page.getByLabel("SSH 私钥", { exact: true }).fill(key);
    await page.getByRole("button", { name: "检查已管理规则的撤销" }).click();
    await preview.getByRole("button", { name: "确认撤销上述规则" }).click();
    await expect(preview).toContainText("本系统记录的规则已撤销");
  } finally {
    await removeFolder(folder);
  }
});
