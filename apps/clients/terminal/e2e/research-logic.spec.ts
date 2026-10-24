import { test, expect } from "./test";
import { rpc, taskRpc } from "./dataset-fixture";
import { openView } from "./navigation-helper";
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
test("prior fictional expectations remain fixed through actual causal trial and page reload", async ({
  page,
}) => {
  test.setTimeout(120000);
  page.setDefaultTimeout(10000);
  expect(process.env.ASTERION_TEST_NODE_ISOLATED).toBe("1");
  await page.goto("/");
  await openView(page, "logic");
  const workspace = page.getByRole("region", { name: "期货研究", exact: true });
  await expect(workspace.getByText("研究服务已连接", { exact: true })).toBeVisible();
  await workspace.getByLabel("名称", { exact: true }).fill("合成双均线逻辑");
  await workspace
    .getByLabel("研究目的", { exact: true })
    .fill("先存虚构预期，不声称收益或实盘许可");
  await workspace.getByRole("button", { name: "保存不可变规则", exact: true }).click();
  await expect(
    workspace.getByRole("heading", { name: "合成双均线逻辑", exact: true }),
  ).toBeVisible();
  const definitions = (await rpc(page.request, "research.logic.list", { after: "", limit: 30 }))
    .research_logic.definitions;
  expect(definitions).toHaveLength(1);
  const definition = definitions[0];
  await workspace.getByLabel("样例名称", { exact: true }).fill("先写预期故意不符");
  await workspace
    .getByLabel("适用范围", { exact: true })
    .fill("四个虚构完成收盘，仅检验因果目标方向");
  await workspace
    .getByLabel("虚构收盘与事先预期", { exact: true })
    .fill("10,warmup\n10,warmup\n13,-1\n7,-1");
  await workspace.getByRole("button", { name: "保存事先预期，不运行", exact: true }).click();
  await expect(
    workspace.getByRole("heading", { name: "先写预期故意不符", exact: true }),
  ).toBeVisible();
  const saved = (
    await rpc(page.request, "research.logic.examples", {
      after: "",
      limit: 30,
      definition_id: definition.object.id,
    })
  ).research_logic.examples;
  expect(saved).toHaveLength(1);
  const example = saved[0];
  const tasks = async () => (await rpc(page.request, "runtime.snapshot")).research_service.tasks;
  expect(
    (await tasks()).filter((task: { kind: string }) => task.kind === "logic_trial"),
  ).toHaveLength(0);
  await expect(workspace.getByLabel("虚构收盘与事先预期", { exact: true })).toHaveCount(0);
  await workspace.getByLabel("试算任务标识", { exact: true }).fill("logic-browser-trial");
  await workspace.getByRole("button", { name: "运行固定样例试算", exact: true }).click();
  await expect
    .poll(
      async () =>
        (await tasks()).find((task: { id: string }) => task.id === "logic-browser-trial")?.state,
      { timeout: 30000 },
    )
    .toBe("succeeded");
  await workspace.getByRole("button", { name: "读取原试算结果", exact: true }).click();
  await expect(workspace.getByText("不匹配", { exact: true })).toHaveCount(1);
  const rows = workspace.getByRole("row");
  expect(await rows.count()).toBe(5);
  await expect(rows.nth(3)).toContainText("13");
  await expect(rows.nth(3)).toContainText("不匹配");
  const report = (
    await taskRpc(page.request, "research", "task.result.read", { id: "logic-browser-trial" })
  ).task_result;
  expect(report.kind).toBe("logic_trial");
  expect(report.result.all_match).toBe(false);
  expect(report.result.matched).toBe(3);
  expect(report.result.decisions).toBe(2);
  expect(report.experiment.example.object).toEqual(example.object);
  expect(report.experiment.example.observations[2].expected.value).toBe(-1);
  expect(report.result.rows[2].actual.value).toBe(1);
  await workspace.getByLabel("停用原因", { exact: true }).fill("停止新试算，保留原预期和结果");
  await workspace.getByRole("button", { name: "停用新试算，保留原证据", exact: true }).click();
  await expect(
    workspace.getByRole("button", { name: "运行固定样例试算", exact: true }),
  ).toBeDisabled();
  await page.reload();
  await openView(page, "logic");
  await workspace.getByRole("button", { name: "合成双均线逻辑", exact: true }).click();
  await workspace.getByRole("button", { name: "先写预期故意不符", exact: true }).click();
  await workspace.getByLabel("试算任务标识", { exact: true }).fill("logic-browser-trial");
  await workspace.getByRole("button", { name: "读取原试算结果", exact: true }).click();
  await expect(workspace.getByText("不匹配", { exact: true })).toHaveCount(1);
  const items = (await rpc(page.request, "research.logic.list", { after: "", limit: 30 }))
    .research_logic.definitions;
  expect(items[0].object).toEqual(definition.object);
  expect(items[0].retirement_reason).not.toBe("");
  const reopened = (
    await taskRpc(page.request, "research", "task.result.read", { id: "logic-browser-trial" })
  ).task_result;
  expect(reopened).toEqual(report);
  const prior = (await rpc(page.request, "research.logic.example.read", { id: example.object.id }))
    .research_logic.example;
  expect(prior).toEqual(example);
  await expect(
    workspace.getByText(
      "预期不符是比较结果，不是执行失败；全部匹配也不证明策略有效。只有预热的样例未检验方向。",
      { exact: true },
    ),
  ).toBeVisible();
  const state = await rpc(page.request, "runtime.snapshot");
  await rpc(page.request, "node.action", {
    id: "local",
    service: state.data.service,
    action: "stop",
  });
  await rpc(page.request, "node.action", {
    id: "local",
    service: state.research_service.service,
    action: "restart",
  });
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "runtime.snapshot")).nodes
          .find((node: { id: string }) => node.id === "local")
          .health.services.find(
            (value: { id: string }) => value.id === state.research_service.service,
          ).health,
      { timeout: 20000 },
    )
    .toBe("ready");
  expect(
    (await taskRpc(page.request, "research", "task.result.read", { id: "logic-browser-trial" }))
      .task_result,
  ).toEqual(report);
  await page.goto("/?screen=settings&category=resources");
  const catalogue = page.getByRole("region", { name: "业务资产目录", exact: true });
  await catalogue
    .getByRole("combobox", { name: "资产所属域", exact: true })
    .selectOption("research");
  for (const [kind, name] of [
    ["logic_definition", "合成双均线逻辑"],
    ["logic_case", "先写预期故意不符"],
  ]) {
    await catalogue.getByRole("combobox", { name: "资产类别", exact: true }).selectOption(kind);
    await catalogue.getByRole("button", { name: "读取资产目录", exact: true }).click();
    await expect(catalogue.getByText(name, { exact: true })).toBeVisible();
    await expect(
      catalogue.getByRole("button", { name: "查看引用与维护", exact: true }),
    ).toHaveCount(0);
  }
  for (const kind of ["logic_trial", "logic_trial_result"]) {
    const resources = (
      await rpc(page.request, "resource.catalogue", { domain: "research", kind, after: "" })
    ).resources;
    expect(
      resources.items.some(
        (entry: { object: { id: string } }) => entry.object.id === "logic-browser-trial",
      ),
    ).toBe(true);
  }
  await rpc(page.request, "node.action", {
    id: "local",
    service: state.research_service.service,
    action: "stop",
  });
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "runtime.snapshot")).nodes
          .find((node: { id: string }) => node.id === "local")
          .health.services.find(
            (value: { id: string }) => value.id === state.research_service.service,
          ).pid,
      { timeout: 10000 },
    )
    .toBe(0);
  const temporary = mkdtempSync(join(tmpdir(), "asterion-logic-browser-"));
  try {
    const backup = join(temporary, "backup"),
      restored = join(temporary, "restored");
    await page.goto("/?screen=settings&category=backup");
    const maintenance = page.getByRole("region", { name: "数据备份与恢复", exact: true });
    await maintenance
      .getByRole("checkbox", { name: new RegExp(state.research_service.service) })
      .check();
    await maintenance.getByLabel("新备份目录", { exact: true }).fill(backup);
    await maintenance.getByRole("button", { name: "预览备份", exact: true }).click();
    await maintenance
      .getByRole("checkbox", { name: "已核对所选范围、缺失依赖和新目录", exact: true })
      .check();
    await maintenance.getByRole("button", { name: "保存备份", exact: true }).click();
    await expect(maintenance).toContainText("备份已保存并核验");
    await maintenance.getByLabel("已有备份目录", { exact: true }).fill(backup);
    await maintenance.getByRole("button", { name: "核验备份", exact: true }).click();
    await maintenance.getByLabel("新恢复目录", { exact: true }).fill(restored);
    await maintenance
      .getByRole("checkbox", { name: "确认恢复到独立新目录，不覆盖原数据或恢复授权", exact: true })
      .check();
    await maintenance.getByRole("button", { name: "恢复到新目录", exact: true }).click();
    await expect(maintenance).toContainText("已恢复到独立目录并核验，未启动服务");
    const manifest = (await rpc(page.request, "system.backup.inspect", { backup: restored })).backup
      .manifest;
    expect(manifest.owners).toHaveLength(1);
    expect(manifest.dependencies.every((item: { included: boolean }) => item.included)).toBe(true);
    expect(
      manifest.dependencies.some(
        (item: { object: { kind: string; id: string } }) =>
          item.object.kind === "logic_case" && item.object.id === example.object.id,
      ),
    ).toBe(true);
  } finally {
    rmSync(temporary, { recursive: true, force: true });
  }
});
