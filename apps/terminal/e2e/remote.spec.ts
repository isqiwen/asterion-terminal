import { openSettingsWindow, closeSettingsWindow } from "./settings-helper";
import { test, expect } from "@playwright/test";
import { mkdtemp, mkdir, writeFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { execFileSync, spawn } from "node:child_process";
import { createServer, connect } from "node:net";

test("saved remote profile connects through mTLS and reconnects without stopping server", async ({ page }) => {
  const folder = await mkdtemp(join(tmpdir(), "asterion-remote-e2e-"));
  const suffix = process.platform === "win32" ? ".exe" : "";
  const binary = (name: string) => resolve(__dirname, "../../../build/Debug", name + suffix);
  const account = join(folder, "ledger"); await mkdir(account);
  execFileSync(binary("asterion_test_certificates"), [folder]);
  const probe = createServer();
  await new Promise<void>(done => probe.listen(0, "127.0.0.1", done));
  const port = (probe.address() as { port: number }).port;
  await new Promise<void>(done => probe.close(() => done()));
  const server = spawn(binary("asterion-trading"), ["--mode", "paper", "--session", "paper.e2e", "--bind", "127.0.0.1", "--port", String(port), "--directory", account, "--tls-ca", join(folder, "ca.crt"), "--tls-cert", join(folder, "server.crt"), "--tls-key", join(folder, "server.key")], { stdio: "ignore" });
  try {
    await expect.poll(() => new Promise<boolean>(done => { const socket = connect(port, "127.0.0.1"); socket.on("connect", () => { socket.destroy(); done(true); }); socket.on("error", () => done(false)); })).toBe(true);
    await page.goto("/");
    await expect(page.getByRole("button", { name: "查看服务连接", exact: true })).toBeVisible();
    page = await openSettingsWindow(page);
    await page.getByRole("button", { name: "连接与部署", exact: true }).click();
    await page.getByText("直接连接已部署交易服务", { exact: true }).click();
    for (const [label, value] of [["配置名称", "研究服务器"], ["服务器地址", "localhost"], ["端口", String(port)], ["会话标识", "paper.e2e"], ["服务端 CA 文件", join(folder, "ca.crt")], ["客户端证书文件", join(folder, "client.crt")], ["客户端私钥文件", join(folder, "client.key")]]) await page.getByLabel(label, { exact: true }).fill(value);
    await page.getByRole("button", { name: "保存配置", exact: true }).click();
    await page.reload();
    await page.getByRole("button", { name: "连接与部署", exact: true }).click();
    await page.getByText("直接连接已部署交易服务", { exact: true }).click();
    await page.getByLabel("已保存配置").selectOption("研究服务器");
    await page.getByRole("button", { name: "连接已保存配置", exact: true }).click();
    await expect(page.getByRole("status").filter({ hasText: "已连接 · localhost" })).toBeVisible();
    await page.screenshot({ path: join(__dirname, "../test-results/remote-connection.png") });
    page = await closeSettingsWindow(page);
    await page.getByRole("button", { name: "数据", exact: true }).click();
    const csv = join(folder, "ticks.csv"); await writeFile(csv, "timestamp_ns,price,quantity\n100,100,1\n200,99,1\n");
    for (const [label, value] of [["CSV 文件路径", csv], ["品种代码", "rb"], ["实际合约", "rb2610"], ["交割月份", "2026-10"], ["价格步长", "1"], ["每手乘数", "10"]]) await page.getByLabel(label, { exact: true }).fill(value);
    await page.getByRole("button", { name: "校验并预览", exact: true }).click();
    await expect(page.getByText("文件校验完成，可在市场工作区查看历史成交。")).toBeVisible();
    await page.getByRole("button", { name: "交易", exact: true }).click();
    await expect(page.getByLabel("交易记录目录", { exact: true })).toHaveCount(0);
    for (const [label, value] of [["初始模拟资金", "2000"], ["每手保证金", "100"], ["每手开仓费", "2"], ["每手平今费", "3"], ["每手平昨费", "4"], ["单笔数量上限", "100"], ["总持仓量上限", "100"], ["在途委托数上限", "100"]]) await page.getByLabel(label, { exact: true }).fill(value);
    await page.getByRole("button", { name: "创建模拟会话", exact: true }).click();
    await expect(page.getByTestId("paper-balance")).toHaveText("2000 CNY");
    await page.getByRole("button", { name: "回放下一笔", exact: true }).click();
    await expect(page.getByText("1 / 2 笔", { exact: true })).toBeVisible();
    page = await openSettingsWindow(page);
    await page.getByRole("button", { name: "连接与部署", exact: true }).click();
    await page.getByText("直接连接已部署交易服务", { exact: true }).click();
    await page.getByRole("button", { name: "重新连接", exact: true }).click();
    await expect(page.getByRole("status").filter({ hasText: "已连接 · localhost" })).toBeVisible();
    expect(server.exitCode).toBeNull();
    page = await closeSettingsWindow(page);
    await expect(page.getByText("1 / 2 笔", { exact: true })).toBeVisible();
    await page.getByRole("button", { name: "断开连接", exact: true }).click();
    await expect(page.getByLabel("交易记录目录", { exact: true })).toBeVisible();
    expect(server.exitCode).toBeNull();
  } finally {
    await page.request.post("/__asterion/api", { data: { version: 1, method: "paper.close", params: {} } });
    if (server.exitCode === null) { server.kill(); await new Promise<void>(done => server.once("exit", () => done())); }
    await rm(folder, { recursive: true, force: true });
  }
});
