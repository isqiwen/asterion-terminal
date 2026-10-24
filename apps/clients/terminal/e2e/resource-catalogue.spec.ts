import { test, expect } from "./test";
import { rpc, seedHistory, taskRpc } from "./dataset-fixture";

test("owner catalogue reaches exact maintenance previews without releasing or changing formal evidence", async ({
  page,
}) => {
  const seeded = await seedHistory(
    page.request,
    Array.from({ length: 40 }, (_, i) => 100 + i + (i % 3)),
    "catalogue-browser",
  );
  await rpc(page.request, "data.dataset.select", seeded);
  await rpc(page.request, "factor.submit", {
    id: "catalogued-factor",
    series: { kind: "bars" },
    factor: "momentum",
    lookbacks: [1],
    horizon: 1,
    evaluation: { mode: "full_sample" },
  });
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "runtime.snapshot")).research_service.tasks.find(
          (task: { id: string }) => task.id === "catalogued-factor",
        )?.state,
      { timeout: 20000 },
    )
    .toBe("succeeded");
  const original = (
    await taskRpc(page.request, "research", "task.result.read", { id: "catalogued-factor" })
  ).task_result;
  const holds = (
    await rpc(page.request, "data.history.usage", { id: seeded.source_dataset_ids[0] })
  ).history_usage;
  expect(holds.consumers).toHaveLength(1);
  await page.goto("/?screen=settings&category=resources");
  const panel = page.getByRole("region", { name: "业务资产目录", exact: true });
  await panel.getByRole("button", { name: "读取资产目录", exact: true }).click();
  const minute = panel.getByRole("row").filter({ hasText: seeded.source_dataset_ids[0] });
  await expect(minute).toBeVisible();
  await expect(minute).toContainText("未知");
  await minute.getByRole("button", { name: "查看引用与维护", exact: true }).click();
  const inspection = panel.getByRole("region", { name: "资源引用与维护预览", exact: true });
  await expect(inspection).toContainText("catalogued-factor");
  await panel.getByLabel("资产类别").selectOption("provider_module");
  await panel.getByRole("button", { name: "读取资产目录", exact: true }).click();
  const programRows = panel.getByRole("row").filter({ hasText: "programs/" });
  await expect(programRows.first()).toBeVisible();
  await expect(programRows.first()).toContainText("程序工件维护尚未实现");
  await expect(programRows.first().getByRole("button")).toHaveCount(0);
  await panel.getByLabel("资产所属域").selectOption("research");
  await panel.getByLabel("资产类别").selectOption("factor_result");
  await panel.getByRole("button", { name: "读取资产目录", exact: true }).click();
  const result = panel.getByRole("row").filter({ hasText: "catalogued-factor" });
  await expect(result).toContainText(original.task.result_digest);
  await result.getByRole("button", { name: "查看引用与维护", exact: true }).click();
  await expect(inspection).toContainText(original.task.result_digest);
  await expect(inspection).toContainText("可进入所属域确认");
  const listedPage = (
    await rpc(page.request, "resource.catalogue", {
      domain: "research",
      kind: "factor_result",
      after: "",
    })
  ).resources;
  const exact = listedPage.items.find(
    (entry: { object: { id: string } }) => entry.object.id === "catalogued-factor",
  );
  expect(exact).toBeTruthy();
  const listed = exact.object;
  const wrong = await page.request.post("/__asterion/api", {
    data: {
      version: 1,
      method: "resource.inspect",
      params: { object: { ...listed, node: "wrong-owner" } },
    },
  });
  expect((await wrong.json()).error?.code).toBe("conflict");
  const after = (
    await taskRpc(page.request, "research", "task.result.read", { id: "catalogued-factor" })
  ).task_result;
  expect(after).toEqual(original);
  expect(after.task.retired).not.toBe(true);
  expect(
    (await rpc(page.request, "data.history.usage", { id: seeded.source_dataset_ids[0] }))
      .history_usage.consumers,
  ).toEqual(holds.consumers);
  await expect(panel.getByRole("checkbox")).toHaveCount(0);
});
