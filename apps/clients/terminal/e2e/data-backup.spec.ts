import { test, expect } from "./test";
import { rpc, seedHistory } from "./dataset-fixture";
import { mkdtempSync, rmSync, readFileSync, existsSync, mkdirSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { createHash } from "node:crypto";

test("@journey local cold backup verifies scope and restores independently without starting services", async ({
  page,
}, testInfo) => {
  await seedHistory(page.request, [100, 101, 102], "cold-browser");
  const initial = await rpc(page.request, "runtime.snapshot");
  const local = initial.nodes.find((node: { id: string }) => node.id === "local");
  const services = local.health.services.filter((item: { kind: string }) =>
    ["data", "research"].includes(item.kind),
  );
  const selected = services.map((item: { id: string }) => item.id);
  const rejected = await page.request.post("/__asterion/api", {
    data: {
      version: 1,
      method: "system.backup.preview",
      params: {
        operation_id: "running-owner",
        services: selected,
        destination: testInfo.outputPath("running-backup"),
      },
    },
  });
  expect((await rejected.json()).error?.code).toBe("conflict");
  for (const service of services)
    await rpc(page.request, "node.action", { service: service.id, action: "stop" });
  await expect
    .poll(
      async () => {
        const state = await rpc(page.request, "runtime.snapshot");
        return state.nodes
          .find((node: { id: string }) => node.id === "local")
          .health.services.filter((item: { id: string }) => selected.includes(item.id))
          .every(
            (item: { desired_running: boolean; pid: number }) => !item.desired_running && !item.pid,
          );
      },
      { timeout: 10000 },
    )
    .toBe(true);
  const data = services.find((item: { kind: string }) => item.kind === "data");
  const ledger = join(data.directory, "retentions.sqlite");
  const digest = () => createHash("sha256").update(readFileSync(ledger)).digest("hex");
  const original = digest();
  const directory = mkdtempSync(join(tmpdir(), "asterion-cold-browser-"));
  try {
    const backup = join(directory, "backup"),
      restore = join(directory, "restore");
    await page.goto("/?screen=settings&category=backup");
    const panel = page.getByRole("region", { name: "数据备份与恢复", exact: true });
    for (const service of services)
      await panel.getByRole("checkbox", { name: new RegExp(service.id) }).check();
    await panel.getByLabel("新备份目录", { exact: true }).fill(backup);
    await panel.getByRole("button", { name: "预览备份", exact: true }).click();
    await expect(panel.getByRole("button", { name: "保存备份", exact: true })).toBeDisabled();
    await panel.getByRole("checkbox", { name: "已核对所选范围、缺失依赖和新目录" }).check();
    await panel.getByRole("button", { name: "保存备份", exact: true }).click();
    await expect(panel).toContainText("备份已保存并核验");
    expect(existsSync(join(backup, "complete.sha256"))).toBe(true);
    await panel.getByLabel("已有备份目录", { exact: true }).fill(backup);
    await panel.getByRole("button", { name: "核验备份", exact: true }).click();
    await panel.getByLabel("新恢复目录", { exact: true }).fill(restore);
    await expect(panel.getByRole("button", { name: "恢复到新目录", exact: true })).toBeDisabled();
    await panel
      .getByRole("checkbox", { name: "确认恢复到独立新目录，不覆盖原数据或恢复授权" })
      .check();
    await panel.getByRole("button", { name: "恢复到新目录", exact: true }).click();
    await expect(panel).toContainText("已恢复到独立目录并核验，未启动服务");
    expect(digest()).toBe(original);
    const verified = await rpc(page.request, "system.backup.inspect", { backup: restore });
    expect(verified.backup.manifest.owners).toHaveLength(2);
    const final = await rpc(page.request, "runtime.snapshot");
    expect(
      final.nodes
        .find((node: { id: string }) => node.id === "local")
        .health.services.filter((item: { id: string }) => selected.includes(item.id))
        .every(
          (item: { desired_running: boolean; pid: number }) => !item.desired_running && !item.pid,
        ),
    ).toBe(true);
    await page.screenshot({ path: testInfo.outputPath("cold-backup.png"), fullPage: true });
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});

test("unfinished cold backup resumes only after revalidating its persisted scope", async ({
  page,
}) => {
  const state = await rpc(page.request, "runtime.snapshot");
  const selected = state.nodes
    .find((node: { id: string }) => node.id === "local")
    .health.services.filter((service: { kind: string }) =>
      ["data", "research"].includes(service.kind),
    )
    .map((service: { id: string }) => service.id);
  const prepared = await rpc(page.request, "system.backup.preview", {
    operation_id: "interrupted-browser-cut",
    services: selected,
    destination: tmpdir() + "/unused-backup-preview",
  });
  const root = mkdtempSync(join(tmpdir(), "asterion-backup-resume-"));
  const partial = join(root, "unfinished");
  try {
    mkdirSync(partial);
    writeFileSync(join(partial, "backup.lock"), "");
    writeFileSync(join(partial, "plan.json"), JSON.stringify(prepared.backup.manifest));
    const incomplete = await page.request.post("/__asterion/api", {
      data: { version: 1, method: "system.backup.inspect", params: { backup: partial } },
    });
    expect((await incomplete.json()).error?.code).toBe("invalid_request");
    expect(existsSync(join(partial, "complete.sha256"))).toBe(false);
    await page.goto("/?screen=settings&category=backup");
    const panel = page.getByRole("region", { name: "数据备份与恢复", exact: true });
    await panel.getByLabel("已有备份目录", { exact: true }).fill(partial);
    await panel.getByRole("button", { name: "预览继续未完成备份", exact: true }).click();
    await expect(panel.getByLabel("新备份目录", { exact: true })).toHaveValue(partial);
    expect(existsSync(join(partial, "complete.sha256"))).toBe(false);
    await panel.getByRole("checkbox", { name: "已核对所选范围、缺失依赖和新目录" }).check();
    await panel.getByRole("button", { name: "保存备份", exact: true }).click();
    await expect(panel).toContainText("备份已保存并核验");
    const complete = await rpc(page.request, "system.backup.inspect", { backup: partial });
    expect(complete.backup.manifest.backup_id).toBe("interrupted-browser-cut");
    expect(existsSync(join(partial, "complete.sha256"))).toBe(true);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});
