import { taskRpc } from "./dataset-fixture";
import { openView } from "./navigation-helper";
import { test, expect } from "./test";
import { rpc, seedHistory } from "./dataset-fixture";

test("F05 derives a reviewed daily momentum window and cancellation leaves no task", async ({
  page,
}) => {
  test.setTimeout(90000);
  const days = Array.from({ length: 56 }, (_, i) =>
    new Date(Date.UTC(2026, 7, 20 + i)).toISOString().slice(0, 10),
  ).filter(day => ![0, 6].includes(new Date(`${day}T00:00:00Z`).getUTCDay()));
  const seeded = await seedHistory(page.request, [100], "window-original", {
    tablesOnly: true,
    dailyDays: days,
  });
  await rpc(page.request, "factor.submit", {
    id: "window-original",
    series: {
      kind: "tables",
      snapshot: seeded.daily.snapshot,
      daily_source: seeded.daily.daily_source,
      calendar_source: seeded.daily.calendar_source,
      end_day: days.at(-1)!,
      ranges: [{ begin_day: days[0], instruments: ["SHFE/rb/2026-10"] }],
    },
    factor: "momentum",
    lookbacks: [2],
    horizon: 1,
    evaluation: { mode: "full_sample" },
  });
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "runtime.snapshot")).research_service.tasks.find(
          (t: { id: string }) => t.id === "window-original",
        )?.state,
    )
    .toBe("succeeded");
  const original = (
    await taskRpc(page.request, "research", "task.result.read", { id: "window-original" })
  ).task_result;
  const ids = async () =>
    (await rpc(page.request, "runtime.snapshot")).research_service.tasks.map(
      (t: { id: string }) => t.id,
    );
  const before = await ids();
  await page.goto("/");
  await openView(page, "factor");
  const workspace = page.getByRole("region", { name: "期货研究", exact: true });
  await workspace
    .getByRole("region", { name: "研究任务", exact: true })
    .getByRole("row")
    .filter({ hasText: "window-original" })
    .getByRole("button", { name: "查看结果", exact: true })
    .click();
  const reuse = workspace.getByRole("region", { name: "按此日线实验创建", exact: true });
  async function review() {
    await reuse.getByRole("button", { name: "修改回看窗口并派生", exact: true }).click();
    await reuse.getByLabel("派生回看日线数", { exact: true }).fill("5");
    await reuse.getByLabel("沿用原固定快照", { exact: true }).check();
    await reuse.getByRole("button", { name: "核对参数派生", exact: true }).click();
    await expect(reuse.getByRole("region", { name: "参数派生确认", exact: true })).toContainText(
      "2 → 5",
    );
    await expect(reuse).toContainText(seeded.daily.snapshot);
    await expect(reuse).toContainText(seeded.daily.daily_source);
  }
  await review();
  await reuse.getByRole("button", { name: "取消参数派生", exact: true }).click();
  expect(await ids()).toEqual(before);
  await review();
  await reuse.getByRole("button", { name: "确认创建派生因子任务", exact: true }).click();
  const id = (await reuse.getByRole("status").locator("code").textContent())!;
  expect(id).not.toBe("window-original");
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "runtime.snapshot")).research_service.tasks.find(
          (t: { id: string }) => t.id === id,
        )?.state,
    )
    .toBe("succeeded");
  const derived = (await taskRpc(page.request, "research", "task.result.read", { id })).task_result;
  expect(derived.experiment.lookbacks).toEqual([5]);
  expect(derived.experiment.series).toEqual(original.experiment.series);
  expect(derived.experiment.dataset_revision).toBe(original.experiment.dataset_revision);
  expect(derived.experiment.horizon).toBe(original.experiment.horizon);
  expect(derived.experiment.evaluation).toEqual(original.experiment.evaluation);
  expect(derived.task.metadata.source_id).toBe("window-original");
  const unchanged = (
    await taskRpc(page.request, "research", "task.result.read", { id: "window-original" })
  ).task_result;
  expect(unchanged.experiment).toEqual(original.experiment);
  expect(unchanged.result).toEqual(original.result);
  const comparison = workspace.getByRole("region", { name: "日线因子比较", exact: true });
  for (const selected of ["window-original", id]) {
    await comparison.getByRole("checkbox", { name: new RegExp(selected) }).check();
  }
  await comparison.getByRole("button", { name: "读取并比较", exact: true }).click();
  const table = comparison.getByRole("table", { name: "因子实验对照", exact: true });
  await expect(table).toBeVisible();
  await expect(table).toContainText("同口径");
  await expect(
    table
      .getByRole("row")
      .filter({ has: page.getByRole("rowheader", { name: "候选窗口", exact: true }) }),
  ).toContainText("5");
});
