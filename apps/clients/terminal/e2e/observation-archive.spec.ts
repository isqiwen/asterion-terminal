import { test, expect } from "./test";
import { ctpConnection, rpc, openData } from "./dataset-fixture";
import { openView } from "./navigation-helper";

test("Market observation is published by Data and remains exactly readable without its producer", async ({
  page,
}) => {
  await ctpConnection(page.request, "observation-fixture", {
    broker_id: "test",
    user_id: "fixture",
    market: true,
  });
  await rpc(page.request, "market.local");
  await rpc(page.request, "market.connect", {
    password: "synthetic-ui-observation",
    instruments: [{ venue: "SHFE", symbol: "rb2610" }],
  });
  await openData(page.request);
  await expect
    .poll(
      async () => {
        const state = await rpc(page.request, "runtime.snapshot");
        return state.market?.phase === "connected" && state.market.out_of_order >= 2;
      },
      { timeout: 15000 },
    )
    .toBe(true);
  await page.goto("/");
  // Archiving observed quotes is a Data operation, on the archive page.
  await openView(page, "records");
  const panel = page.getByRole("region", { name: "观察归档", exact: true });
  await panel.getByRole("button", { name: "固定观察预览", exact: true }).click();
  await expect(
    panel.getByRole("region", { name: "Market 固定观察预览", exact: true }),
  ).toBeVisible();
  await expect(panel).toContainText("不是完整历史");
  await expect(panel).toContainText("观察曾中断，不能证明连续覆盖");
  await expect(panel.getByRole("button", { name: "接纳并发布归档", exact: true })).toBeDisabled();
  await panel.getByLabel("确认将此固定观察交给当前 Data", { exact: true }).check();
  const id = await panel.getByLabel("归档操作 ID", { exact: true }).inputValue();
  await panel.getByRole("button", { name: "接纳并发布归档", exact: true }).click();
  const fixed = panel.getByRole("region", { name: "Data 固定观察原件", exact: true });
  await expect(fixed).toBeVisible();
  await expect(panel.getByRole("region", { name: "归档状态", exact: true })).toContainText(
    "已发布固定原件",
  );
  const original = (await rpc(page.request, "data.observation.read", { id })).observation_archive;
  expect(original.capture.complete_history).toBe(false);
  expect(original.capture.minutes.interrupted).toBe(true);
  const rows = await fixed.getByRole("table", { name: "固定观察分钟" }).innerText();
  expect(original.capture.minutes.bars.length).toBeGreaterThan(0);
  await rpc(page.request, "node.action", {
    service: original.source.service,
    action: "stop",
  });
  await panel.getByRole("button", { name: "从 Data 读取固定原件", exact: true }).click();
  await expect(fixed.getByRole("table", { name: "固定观察分钟" })).toHaveText(rows, {
    useInnerText: true,
  });
  expect((await rpc(page.request, "data.observation.read", { id })).observation_archive).toEqual(
    original,
  );
  await rpc(page.request, "node.action", {
    service: original.archive.service,
    action: "stop",
  });
  await openData(page.request);
  await page.reload();
  // The shared fixture acknowledges startup notices; SetupGate tests verify their exact text.
  await openView(page, "records");
  await panel.getByLabel("归档操作 ID", { exact: true }).fill(id);
  await panel.getByRole("button", { name: "从 Data 读取固定原件", exact: true }).click();
  await expect(fixed).toBeVisible();
  expect((await rpc(page.request, "data.observation.read", { id })).observation_archive).toEqual(
    original,
  );
  const detached = await rpc(page.request, "runtime.snapshot");
  expect(
    detached.nodes
      .flatMap(
        (node: { health?: { services?: { id: string; state: string }[] } }) =>
          node.health?.services ?? [],
      )
      .find((service: { id: string }) => service.id === original.source.service)?.state,
  ).toBe("stopped");
  expect(await page.evaluate(() => JSON.stringify(localStorage))).not.toContain(
    "synthetic-ui-observation",
  );
});
