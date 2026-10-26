import { test, expect } from "./test";
import { ctpConnection, rpc } from "./dataset-fixture";
import { openView } from "./navigation-helper";
import { mkdtempSync, rmSync, readFileSync, existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { createHash } from "node:crypto";

test("fixed account facts publish through the UI without trading and survive source change and restart", async ({
  page,
}) => {
  test.setTimeout(90000);
  const account = "facts-account";
  await ctpConnection(page.request, account, {
    broker_id: "9999",
    user_id: "000001",
    trade_front: "tcp://127.0.0.1:41205",
  });
  await rpc(page.request, "market.catalog", {
    account,
    password: "catalog-only",
    auth_code: "auth-code",
  });
  await expect
    .poll(async () => (await rpc(page.request, "runtime.snapshot")).market.catalog.phase)
    .toBe("ready");
  await rpc(page.request, "live.create", {
    account,
    node: "local",
    service: "",
    environment: "simulation",
    max_order_quantity: "5",
    max_gross_quantity: "10",
    max_working_orders: "2",
    max_price_deviation: "0.02",
    contracts: [{ venue: "SHFE", symbol: "rb2610" }],
  });
  await page.goto("/");
  await openView(page, "trading");
  const panel = page.getByRole("region", { name: "固定账户事实", exact: true });
  await panel.getByLabel("账户事实操作 ID", { exact: true }).fill("fixed-browser-facts");
  await expect(panel.getByRole("button", { name: "固定或恢复原操作", exact: true })).toBeDisabled();
  await panel.getByLabel("确认保留本次固定事实及覆盖限制", { exact: true }).check();
  await panel.getByRole("button", { name: "固定或恢复原操作", exact: true }).click();
  await expect(panel).toContainText("账户事实已发布");
  await expect(panel).toContainText("未取得资金观察，不能当作零余额");
  await expect(panel).toContainText("缺少资金币种来源字段");
  const original = (
    await rpc(page.request, "live.facts.status", { account, id: "fixed-browser-facts" })
  ).trading_facts;
  expect(original.capture.phase).toBe("disconnected");
  expect(original.capture.funds).toBeNull();
  expect(original.capture.complete_history).toBe(false);
  expect(original.capture.atomic_ledger_cut).toBe(false);
  expect(original.capture.broker).toBeUndefined();
  expect(original.capture.authorization).toBeUndefined();
  const before = (await rpc(page.request, "runtime.snapshot")).live[account];
  expect(before.session.authorization).toBeNull();
  expect(before.session.orders).toEqual([]);
  // The selected SDK is the test-only CTP implementation. This changes cached
  // source observations but never authorizes or sends an order.
  await rpc(page.request, "live.connect", {
    account,
    password: "facts-browser-secret",
    auth_code: "auth-code",
  });
  await expect
    .poll(async () => (await rpc(page.request, "runtime.snapshot")).live[account].session.phase, {
      timeout: 20000,
    })
    .toBe("ready");
  await panel.getByRole("button", { name: "固定或恢复原操作", exact: true }).click();
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "live.facts.status", { account, id: "fixed-browser-facts" }))
          .trading_facts,
    )
    .toEqual(original);
  await panel.getByRole("button", { name: "读取此固定事实", exact: true }).click();
  await expect(panel).toContainText(original.reference.version);
  await expect(panel).toContainText("未取得资金观察，不能当作零余额");
  await rpc(page.request, "live.close", { account });
  await rpc(page.request, "node.action", {
    id: "local",
    service: before.connection.session,
    action: "restart",
  });
  await rpc(page.request, "live.open", { account });
  await page.reload();
  await openView(page, "trading");
  await panel.getByLabel("账户事实操作 ID", { exact: true }).fill("fixed-browser-facts");
  await panel.getByRole("button", { name: "检查原事实操作", exact: true }).click();
  await expect(panel).toContainText(original.reference.version);
  expect(
    (await rpc(page.request, "live.facts.read", { account, reference: original.reference }))
      .trading_facts,
  ).toEqual(original);
  const recovered = (await rpc(page.request, "runtime.snapshot")).live[account].session;
  expect(recovered.authorization).toBeNull();
  expect(recovered.orders).toEqual([]);
  await panel.getByRole("button", { name: "在 Research 审阅此固定事实", exact: true }).click();
  await expect(page.getByLabel("账户审阅任务 ID", { exact: true })).toBeVisible();
  await page.getByLabel("账户审阅任务 ID", { exact: true }).fill("fixed-browser-review");
  await expect(page.getByRole("button", { name: "提交或确认原审阅", exact: true })).toBeEnabled();
  await page.getByRole("button", { name: "提交或确认原审阅", exact: true }).click();
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "runtime.snapshot")).research_service?.tasks.find(
          (task: { id: string }) => task.id === "fixed-browser-review",
        )?.state,
    )
    .toBe("succeeded");
  await page.getByRole("button", { name: "查看结果", exact: true }).click();
  const review = page.getByRole("region", { name: "账户观察审阅结果", exact: true });
  await expect(review).toContainText(original.reference.version);
  await expect(review).toContainText("证据不足以计算完整历史绩效");
  await expect(review).toContainText("未取得资金观察，不能当作零余额");
  const runtime = await rpc(page.request, "runtime.snapshot");
  const research = runtime.research_service;
  const owner = {
    kind: "research",
    node_id: research.node_id,
    service: research.service,
    connection_id: research.connection_id,
  };
  const fixedReview = (
    await rpc(page.request, "task.result.read", { id: "fixed-browser-review", owner })
  ).task_result;
  expect(fixedReview.kind).toBe("account_review");
  expect(fixedReview.experiment.evidence).toEqual(original);
  expect(fixedReview.result.funds_present).toBe(false);
  await rpc(page.request, "live.close", { account });
  await rpc(page.request, "node.action", {
    id: "local",
    service: before.connection.session,
    action: "stop",
  });
  await rpc(page.request, "node.action", {
    id: "local",
    service: research.service,
    action: "restart",
  });
  await page.reload();
  await page
    .getByRole("navigation", { name: "业务工作区", exact: true })
    .getByRole("button", { name: "实盘", exact: true })
    .click();
  await page
    .getByRole("navigation", { name: "子模块视图", exact: true })
    .getByRole("button", { name: "账户核对", exact: true })
    .click();
  await expect(page.getByRole("button", { name: "查看结果", exact: true })).toBeEnabled();
  await page.getByRole("button", { name: "查看结果", exact: true }).click();
  await expect(review).toContainText(original.reference.version);
  await expect(review).toContainText(fixedReview.result.program_artifact);
  const afterRestart = (await rpc(page.request, "runtime.snapshot")).research_service;
  expect(
    (
      await rpc(page.request, "task.result.read", {
        id: "fixed-browser-review",
        owner: {
          kind: "research",
          node_id: afterRestart.node_id,
          service: afterRestart.service,
          connection_id: afterRestart.connection_id,
        },
      })
    ).task_result.result,
  ).toEqual(fixedReview.result);
  const catalogue = (
    await rpc(page.request, "resource.catalogue", {
      domain: "research",
      kind: "research_program",
      after: "",
    })
  ).resources;
  expect(
    catalogue.items.some(
      (entry: { object: { version: string } }) =>
        entry.object.version === fixedReview.result.program_artifact,
    ),
  ).toBe(true);
  const inputs = (
    await rpc(page.request, "resource.catalogue", {
      domain: "research",
      kind: "account_review",
      after: "",
    })
  ).resources;
  const fixedInput = inputs.items.find(
    (entry: { object: { id: string } }) => entry.object.id === "fixed-browser-review",
  );
  expect(fixedInput.object.version).toBe(fixedReview.input_sha256);
  const inspection = (await rpc(page.request, "resource.inspect", { object: fixedInput.object }))
    .resource_inspection;
  expect(inspection.maintenance.task.kind).toBe("account_review");
  expect(inspection.maintenance.version).toBe(fixedReview.input_sha256);
  const temporary = mkdtempSync(join(tmpdir(), "asterion-trading-evidence-browser-"));
  try {
    const backup = join(temporary, "backup"),
      restore = join(temporary, "restore");
    const selected = [before.connection.session, research.service];
    const running = await page.request.post("/__asterion/api", {
      data: {
        version: 1,
        method: "system.backup.preview",
        params: { operation_id: "running-trading-cut", services: selected, destination: backup },
      },
    });
    expect((await running.json()).error?.code).toBe("conflict");
    expect(existsSync(backup)).toBe(false);
    await rpc(page.request, "node.action", {
      id: "local",
      service: research.service,
      action: "stop",
    });
    const stopped = async () => {
      const services = (await rpc(page.request, "runtime.snapshot")).nodes
        .find((node: { id: string }) => node.id === "local")
        .health.services.filter((service: { id: string }) => selected.includes(service.id));
      return (
        services.length === selected.length &&
        services.every(
          (service: { desired_running: boolean; pid: number }) =>
            !service.desired_running && !service.pid,
        )
      );
    };
    await expect.poll(stopped, { timeout: 10000 }).toBe(true);
    const managed = (await rpc(page.request, "runtime.snapshot")).nodes.find(
      (node: { id: string }) => node.id === "local",
    ).health.services;
    const trader = managed.find(
      (service: { id: string }) => service.id === before.connection.session,
    );
    const researcher = managed.find((service: { id: string }) => service.id === research.service);
    const digest = (path: string) => createHash("sha256").update(readFileSync(path)).digest("hex");
    const sourceLedger = join(trader.directory, "journal.sqlite");
    const researchLedger = join(researcher.directory, "tasks.sqlite");
    const sourceBefore = digest(sourceLedger),
      researchBefore = digest(researchLedger);
    await page.goto("/?screen=settings&category=backup");
    const maintenance = page.getByRole("region", { name: "数据备份与恢复", exact: true });
    for (const id of selected)
      await maintenance.getByRole("checkbox", { name: new RegExp(id) }).check();
    await maintenance.getByLabel("新备份目录", { exact: true }).fill(backup);
    await maintenance.getByRole("button", { name: "预览备份", exact: true }).click();
    await expect(maintenance).toContainText(account);
    await expect(maintenance).toContainText("CTP 仿真");
    await expect(maintenance).toContainText("现有账本不包含完整柜台历史");
    await expect(maintenance.getByRole("button", { name: "保存备份", exact: true })).toBeDisabled();
    await maintenance.getByRole("checkbox", { name: "已核对所选范围、缺失依赖和新目录" }).check();
    await maintenance.getByRole("button", { name: "保存备份", exact: true }).click();
    await expect(maintenance).toContainText("备份已保存并核验");
    const saved = (await rpc(page.request, "system.backup.inspect", { backup })).backup;
    expect(saved.partial).toBe(true); // Complete selected dependencies are distinct from missing historical evidence.
    expect(saved.manifest.owners.map((owner: { domain: string }) => owner.domain).sort()).toEqual([
      "research",
      "trading",
    ]);
    expect(
      saved.manifest.dependencies.every((dependency: { included: boolean }) => dependency.included),
    ).toBe(true);
    expect(
      saved.manifest.files.some((file: { path: string }) => file.path.startsWith("ctp-flow/")),
    ).toBe(false);
    await maintenance.getByLabel("已有备份目录", { exact: true }).fill(backup);
    await maintenance.getByRole("button", { name: "核验备份", exact: true }).click();
    await maintenance.getByLabel("新恢复目录", { exact: true }).fill(restore);
    await expect(
      maintenance.getByRole("button", { name: "恢复到新目录", exact: true }),
    ).toBeDisabled();
    await maintenance
      .getByRole("checkbox", { name: "确认恢复到独立新目录，不覆盖原数据或恢复授权" })
      .check();
    await maintenance.getByRole("button", { name: "恢复到新目录", exact: true }).click();
    await expect(maintenance).toContainText("已恢复到独立目录并核验，未启动服务");
    expect(digest(sourceLedger)).toBe(sourceBefore);
    expect(digest(researchLedger)).toBe(researchBefore);
    const restored = (await rpc(page.request, "system.backup.inspect", { backup: restore })).backup;
    expect(restored.manifest.scopes).toEqual(saved.manifest.scopes);
    expect(
      restored.manifest.dependencies.every(
        (dependency: { included: boolean }) => dependency.included,
      ),
    ).toBe(true);
    expect(await stopped()).toBe(true);
  } finally {
    rmSync(temporary, { recursive: true, force: true });
  }
  expect(await page.evaluate(() => JSON.stringify(localStorage))).not.toContain(
    "facts-browser-secret",
  );
});
