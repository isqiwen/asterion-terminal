import { openSettingsWindow, closeSettingsWindow } from "./settings-helper";
import { test, expect } from "@playwright/test";
import { mkdtemp, mkdir, rm, writeFile, readFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { execFileSync } from "node:child_process";
import { createServer, connect } from "node:net";
async function freePort() { const probe = createServer(); await new Promise<void>(done => probe.listen(0, "127.0.0.1", done)); const port = (probe.address() as { port: number }).port; await new Promise<void>(done => probe.close(() => done())); return port; }
test("Terminal deploys and controls a service through Node Agent", async ({ page }) => {
  test.skip(process.platform !== "linux", "Remote deployment fixture requires a native Linux Agent");
  test.setTimeout(90000);
  const folder = await mkdtemp(join(tmpdir(), "asterion-node-agent-ui-")); const root = join(folder, "node"); await mkdir(root);
  const binary = (name: string) => resolve(__dirname, "../../../build/Debug", name + (process.platform === "win32" ? ".exe" : ""));
  execFileSync(binary("asterion_test_certificates"), [folder]);
  const management = await freePort(), servicePort = await freePort();
  const known = join(folder, "known_hosts"); await writeFile(known, "test fixture only\n");
  try {
    await page.goto("/"); await expect(page.getByRole("button", { name: "查看服务连接", exact: true })).toBeVisible();
    page = await openSettingsWindow(page); await page.getByRole("button", { name: "连接与部署", exact: true }).click();
    await page.getByRole("button", { name: "远程 Linux", exact: true }).click();
    await page.getByText("通过 SSH 添加机器", { exact: true }).click();
    for (const [label, value] of [["机器名称", "research-ui"], ["SSH 地址", "localhost"], ["SSH 端口", "22"], ["SSH 用户", "asterion"], ["已核验 known_hosts 文件", known], ["Agent 管理端口", String(management)]]) await page.getByLabel(label, { exact: true }).fill(value);
    execFileSync("/usr/bin/ssh-keygen", ["-q", "-t", "ed25519", "-N", "", "-f", join(folder, "ssh-key")]);
    const privateKey = await readFile(join(folder, "ssh-key"), "utf8");
    await expect(page.getByLabel("SSH 认证方式", { exact: true })).toHaveCount(0);
    await page.getByLabel("SSH 密钥来源", { exact: true }).selectOption("provided");
    await page.getByLabel("SSH 私钥", { exact: true }).fill(privateKey);
    await page.getByRole("button", { name: "保存机器配置", exact: true }).click();
    expect(await page.evaluate(() => JSON.stringify(localStorage))).not.toContain("PRIVATE KEY");
    await page.getByRole("button", { name: "通过 SSH 安装并连接", exact: true }).click();
    await expect(page.getByLabel("SSH 私钥", { exact: true })).toHaveValue("");
    const table = page.getByRole("table", { name: "节点服务状态" }); await expect(table).toContainText("节点在线");
    await page.getByText("部署服务", { exact: true }).click();
    await page.getByLabel("部署目标").selectOption("research-ui");
    await page.getByLabel("新服务名称").fill("paper-ui"); await page.getByLabel("服务端口").fill(String(servicePort));
    await page.getByRole("button", { name: "上传并部署", exact: true }).click();
    const service = table.getByRole("row").filter({ hasText: "paper-ui" }); await expect(service).toContainText("进程运行", { timeout: 30000 });
    await expect.poll(() => new Promise<boolean>(done => { const socket = connect(servicePort, "127.0.0.1"); socket.on("connect", () => { socket.destroy(); done(true); }); socket.on("error", () => done(false)); })).toBe(true);
    await service.getByRole("button", { name: "连接交易" }).click(); await expect(page.getByText(/业务状态：等待初始化/)).toBeVisible();
    await page.getByRole("button", { name: "断开连接", exact: true }).click();
    await service.getByRole("button", { name: "停止", exact: true }).click(); await expect(service).toContainText("已停止");
    await table.scrollIntoViewIfNeeded(); await page.screenshot({ path: join(__dirname, "../test-results/node-services.png") });
    await service.getByRole("button", { name: "启动", exact: true }).click(); await expect(service).toContainText("进程运行");
    await service.getByRole("button", { name: "停止", exact: true }).click();
    await table.getByRole("row").filter({ hasText: "research-ui" }).getByRole("button", { name: "移除监控" }).click(); await expect(table).not.toContainText("research-ui");
    await page.getByRole("button", { name: "连接已安装节点", exact: true }).click(); await expect(table).toContainText("research-ui"); 
  } finally {
    await page.request.post("/__asterion/api", { data: { version: 1, method: "paper.close", params: {} } });
    await page.request.post("/__asterion/api", { data: { version: 1, method: "node.disconnect", params: { id: "research-ui" } } });
    const pid = Number(await readFile(join(process.env.ASTERION_SSH_FIXTURE!, "pid"), "utf8"));
    try { process.kill(pid, "SIGTERM"); } catch {}
    await rm(folder, { recursive: true, force: true, maxRetries: 10, retryDelay: 300 });
  }
});
