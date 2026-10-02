import { seedDataset, rpc } from "./dataset-fixture";
import { removeFolder } from "./cleanup";
import { openSettingsWindow, closeSettingsWindow } from "./settings-helper";
import { test, expect } from "@playwright/test";
import { mkdtemp, mkdir } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { execFileSync, spawn } from "node:child_process";
import { createServer, connect } from "node:net";

test("saved remote profile connects through mTLS and reconnects without stopping server", async ({
  page,
}) => {
  const folder = await mkdtemp(join(tmpdir(), "asterion-remote-e2e-"));
  const suffix = process.platform === "win32" ? ".exe" : "";
  const binary = (name: string) =>
    resolve(
      process.env.ASTERION_CPP_BUILD ?? resolve(__dirname, "../../../../build/Debug"),
      name + suffix,
    );
  const account = join(folder, "ledger");
  await mkdir(account);
  execFileSync(binary("asterion_test_certificates"), [folder]);
  const probe = createServer();
  await new Promise<void>(done => probe.listen(0, "127.0.0.1", done));
  const port = (probe.address() as { port: number }).port;
  await new Promise<void>(done => probe.close(() => done()));
  const server = spawn(
    binary("asterion-trading"),
    [
      "--mode",
      "paper",
      "--session",
      "paper.e2e",
      "--bind",
      "127.0.0.1",
      "--port",
      String(port),
      "--directory",
      account,
      "--tls-ca",
      join(folder, "ca.crt"),
      "--tls-cert",
      join(folder, "server.crt"),
      "--tls-key",
      join(folder, "server.key"),
    ],
    { stdio: "ignore" },
  );
  try {
    await expect
      .poll(
        () =>
          new Promise<boolean>(done => {
            const socket = connect(port, "127.0.0.1");
            socket.on("connect", () => {
              socket.destroy();
              done(true);
            });
            socket.on("error", () => done(false));
          }),
      )
      .toBe(true);
    await page.goto("/");
    await expect(page.getByRole("button", { name: "查看服务连接", exact: true })).toBeVisible({
      timeout: 60000,
    });
    page = await openSettingsWindow(page);
    await page.getByRole("button", { name: "连接与部署", exact: true }).click();
    await page.getByText("高级：直接连接交易服务", { exact: true }).click();
    await page.getByText("直接连接已部署交易服务", { exact: true }).click();
    for (const [label, value] of [
      ["配置名称", "研究服务器"],
      ["服务器地址", "localhost"],
      ["端口", String(port)],
      ["会话标识", "paper.e2e"],
      ["服务端 CA 文件", join(folder, "ca.crt")],
      ["客户端证书文件", join(folder, "client.crt")],
      ["客户端私钥文件", join(folder, "client.key")],
    ])
      await page.getByLabel(label, { exact: true }).fill(value);
    await page.getByRole("button", { name: "保存配置", exact: true }).click();
    await page.reload();
    await page.getByRole("button", { name: "连接与部署", exact: true }).click();
    await page.getByText("高级：直接连接交易服务", { exact: true }).click();
    await page.getByText("直接连接已部署交易服务", { exact: true }).click();
    await page.getByLabel("已保存配置").selectOption("研究服务器");
    await page.getByRole("button", { name: "连接已保存配置", exact: true }).click();
    await expect(page.getByRole("status").filter({ hasText: "已连接 · localhost" })).toBeVisible();
    await page.screenshot({ path: join(__dirname, "../test-results/remote-connection.png") });
    page = await closeSettingsWindow(page);
    await seedDataset(page.request, [100, 99], "remote");
    await page.reload();
    await page
      .locator(".workspace-tabs")
      .getByRole("button", { name: "研究", exact: true })
      .click();
    await page.getByRole("button", { name: "历史回放", exact: true }).click();
    await expect(page.getByLabel("交易记录目录", { exact: true })).toHaveCount(0);
    await page.getByRole("button", { name: "下一步", exact: true }).click();
    for (const [label, value] of [
      ["初始模拟资金", "2000"],
      ["每手保证金", "100"],
      ["每手开仓费", "2"],
      ["每手平今费", "3"],
      ["每手平昨费", "4"],
      ["单笔数量上限", "100"],
      ["总持仓量上限", "100"],
      ["在途委托数上限", "100"],
    ])
      await page.getByLabel(label, { exact: true }).fill(value);
    await page.getByRole("button", { name: "下一步", exact: true }).click();
    await page.getByRole("button", { name: "创建回放账户", exact: true }).click();
    await expect(page.getByTestId("paper-balance")).toHaveText("2000 CNY");
    const beforeUsage = await rpc(page.request, "runtime.snapshot");
    const datasetId = beforeUsage.datasets[0].source_dataset_ids[0];
    await page
      .locator(".workspace-tabs")
      .getByRole("button", { name: "数据", exact: true })
      .click();
    await page.getByRole("button", { name: "历史数据仓库", exact: true }).click();
    const archive = page.getByRole("region", { name: "历史数据仓库", exact: true });
    await archive
      .locator(`[data-dataset-id="${datasetId}"]`)
      .getByRole("button", { name: "使用情况", exact: true })
      .click();
    const remoteUsage = archive.getByRole("region", { name: "远程回放账户", exact: true });
    await expect(remoteUsage).toContainText("当前直连账户");
    await expect(remoteUsage).toContainText("已检查 1 个账户，发现 1 个引用");
    await expect(remoteUsage.getByRole("table")).toContainText("paper.e2e");
    await expect(remoteUsage.getByRole("table")).toContainText("行情输入");
    await expect(remoteUsage.getByRole("alert")).toHaveCount(0);
    expect((await rpc(page.request, "runtime.snapshot")).paper).toEqual(beforeUsage.paper);
    expect(server.exitCode).toBeNull();
    await remoteUsage.scrollIntoViewIfNeeded();
    await page.screenshot({ path: "build/history-remote-usage-browser.png", fullPage: true });
    await page
      .locator(".workspace-tabs")
      .getByRole("button", { name: "研究", exact: true })
      .click();
    await page.getByRole("button", { name: "历史回放", exact: true }).click();
    await page.getByRole("button", { name: "回放下一根", exact: true }).click();
    await expect(page.getByText("1 / 2 根", { exact: true })).toBeVisible();
    page = await openSettingsWindow(page);
    await page.getByRole("button", { name: "连接与部署", exact: true }).click();
    await page.getByText("高级：直接连接交易服务", { exact: true }).click();
    await page.getByText("直接连接已部署交易服务", { exact: true }).click();
    await page.getByRole("button", { name: "重新连接", exact: true }).click();
    await expect(page.getByRole("status").filter({ hasText: "已连接 · localhost" })).toBeVisible();
    expect(server.exitCode).toBeNull();
    page = await closeSettingsWindow(page);
    await expect(page.getByText("1 / 2 根", { exact: true })).toBeVisible();
    await page.getByRole("button", { name: "断开连接", exact: true }).click();
    await expect(page.getByRole("region", { name: "账户列表", exact: true })).toBeVisible();
    expect(server.exitCode).toBeNull();
  } finally {
    await page.request.post("/__asterion/api", {
      data: { version: 1, method: "paper.close", params: {} },
    });
    if (server.exitCode === null) {
      server.kill();
      await new Promise<void>(done => server.once("exit", () => done()));
    }
    await removeFolder(folder);
  }
});
