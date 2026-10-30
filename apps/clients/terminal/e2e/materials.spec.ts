import { test, expect } from "./test";
import { rpc, openData } from "./dataset-fixture";
import { openView } from "./navigation-helper";
import { resolve, join } from "node:path";
import { mkdtempSync, rmSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";

test("@journey material acquisition uses real Data and Native, preserves source-free reads and independently restores", async ({
  page,
}, info) => {
  test.setTimeout(120000);
  expect(process.env.ASTERION_TEST_NODE_ISOLATED).toBe("1");
  const build = process.env.ASTERION_CPP_BUILD ?? resolve(__dirname, "../../../../build/Debug");
  const path = join(
    build,
    "material-fixtures",
    "asterion_test_material_provider" + (process.platform === "darwin" ? ".dylib" : ".so"),
  );
  const candidate = (await rpc(page.request, "native.plugins.preview", { path })).plugin_candidate;
  expect(candidate.selectable).toBe(true);
  await rpc(page.request, "native.plugins.install", { path, sha256: candidate.sha256 });
  const installed = await rpc(page.request, "runtime.snapshot");
  const base = installed.native_plugins.items
    .filter((p: { managed?: boolean; selectable: boolean }) => !p.managed && p.selectable)
    .map((p: { sha256: string }) => p.sha256);
  const serviceId = installed.data.service;
  async function stop() {
    await rpc(page.request, "node.action", { service: serviceId, action: "stop" });
    await expect
      .poll(
        async () => {
          const state = await rpc(page.request, "runtime.snapshot");
          const rows = state.nodes
            .find((n: { id: string }) => n.id === "local")
            .health.services.filter((s: { id: string }) => s.id === serviceId);
          return rows.length === 1 && !rows[0].desired_running && rows[0].pid === 0;
        },
        { timeout: 10000 },
      )
      .toBe(true);
  }
  async function configure(plugins: string[]) {
    const state = await rpc(page.request, "runtime.snapshot");
    const own = state.nodes
      .find((n: { id: string }) => n.id === "local")
      .health.services.find((s: { id: string }) => s.id === serviceId);
    await rpc(page.request, "node.plugins.configure", {
      service: serviceId,
      revision: own.revision,
      plugins,
    });
    await openData(page.request);
    await expect
      .poll(async () => (await rpc(page.request, "runtime.snapshot")).data.online, {
        timeout: 10000,
      })
      .toBe(true);
  }
  await stop();
  await configure([...new Set([...base, candidate.sha256])]);
  await page.goto("/");
  await openView(page, "materials");
  const panel = page.getByRole("region", { name: "资料", exact: true });
  await panel.getByRole("button", { name: "刷新材料来源", exact: true }).click();
  await expect(panel.getByRole("combobox", { name: "材料来源", exact: true })).toHaveValue(
    "fixture-material",
  );
  await panel.getByLabel("材料操作 ID", { exact: true }).fill("browser-material");
  await panel.getByLabel("来源资源标识", { exact: true }).fill("announcement");
  await panel.getByLabel("本次使用范围", { exact: true }).fill("isolated synthetic research");
  await expect(panel.getByRole("button", { name: "接纳材料取得任务", exact: true })).toBeDisabled();
  await panel
    .getByRole("checkbox", { name: "已核对来源使用范围与本次请求上限，确认取得原件。" })
    .check();
  await panel.getByRole("button", { name: "接纳材料取得任务", exact: true }).click();
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "data.material.task", { id: "browser-material" })).material_task
          .state,
      { timeout: 10000 },
    )
    .toBe("succeeded");
  await panel.getByRole("button", { name: "读取固定材料", exact: true }).click();
  await expect(panel.getByLabel("固定材料正文", { exact: true })).toHaveValue("工厂甲计划检修\n");
  await expect(panel).toContainText("材料取得成功，尚未进行语义核验或情报发布。");
  await expect(panel).toContainText("来源发布时间：未知");
  const original = (await rpc(page.request, "data.material.read", { id: "browser-material" }))
    .material_read;
  expect(original.receipt.request.provider_artifact).toBe(candidate.sha256);
  const catalogue = (
    await rpc(page.request, "resource.catalogue", {
      domain: "data",
      kind: "material_acquisition",
      after: "",
    })
  ).resources;
  expect(catalogue.items.map((r: { object: unknown }) => r.object)).toContainEqual(
    original.receipt.reference,
  );
  const live = await rpc(page.request, "runtime.snapshot");
  const directory = live.nodes
    .find((n: { id: string }) => n.id === "local")
    .health.services.find((s: { id: string }) => s.id === serviceId).directory;
  expect(directory.startsWith(process.env.ASTERION_ROOT + "/")).toBe(true);
  const receiptFile = join(directory, "materials/receipts/browser-material.pb");
  const receiptBytes = readFileSync(receiptFile);
  await stop();
  await configure(base);
  await page.reload();
  await openView(page, "materials");
  await panel.getByLabel("材料操作 ID", { exact: true }).fill("browser-material");
  await panel.getByRole("button", { name: "读取固定材料", exact: true }).click();
  await expect(panel.getByLabel("固定材料正文", { exact: true })).toHaveValue("工厂甲计划检修\n");
  expect((await rpc(page.request, "data.material.sources")).material_sources).toEqual([]);
  await rpc(page.request, "data.material.submit", original.receipt.request);
  expect(
    (await rpc(page.request, "data.material.read", { id: "browser-material" })).material_read,
  ).toEqual(original);
  await stop();
  const temporary = mkdtempSync(join(tmpdir(), "asterion-material-cold-"));
  try {
    const backup = join(temporary, "backup"),
      restore = join(temporary, "restore");
    await page.goto("/?screen=settings&category=backup");
    const maintenance = page.getByRole("region", { name: "数据备份与恢复", exact: true });
    await maintenance.getByRole("checkbox", { name: new RegExp(serviceId) }).check();
    await maintenance.getByLabel("新备份目录", { exact: true }).fill(backup);
    await maintenance.getByRole("button", { name: "预览备份", exact: true }).click();
    await expect(maintenance.getByRole("button", { name: "保存备份", exact: true })).toBeDisabled();
    await maintenance.getByRole("checkbox", { name: "已核对所选范围、缺失依赖和新目录" }).check();
    await maintenance.getByRole("button", { name: "保存备份", exact: true }).click();
    await expect(maintenance).toContainText("备份已保存并核验");
    await maintenance.getByLabel("已有备份目录", { exact: true }).fill(backup);
    await maintenance.getByRole("button", { name: "核验备份", exact: true }).click();
    await maintenance.getByLabel("新恢复目录", { exact: true }).fill(restore);
    await maintenance
      .getByRole("checkbox", { name: "确认恢复到独立新目录，不覆盖原数据或恢复授权" })
      .check();
    await maintenance.getByRole("button", { name: "恢复到新目录", exact: true }).click();
    await expect(maintenance).toContainText("已恢复到独立目录并核验，未启动服务");
    expect(readFileSync(receiptFile)).toEqual(receiptBytes);
    expect(readFileSync(join(restore, "owners/0/materials/receipts/browser-material.pb"))).toEqual(
      receiptBytes,
    );
    expect(
      readFileSync(
        join(restore, "owners/0/materials/objects", original.receipt.content.version),
        "utf8",
      ),
    ).toBe("工厂甲计划检修\n");
    await page.screenshot({ path: info.outputPath("material-restored.png"), fullPage: true });
  } finally {
    rmSync(temporary, { recursive: true, force: true });
  }
});
