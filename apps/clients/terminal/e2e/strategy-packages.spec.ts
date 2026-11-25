import { execFileSync } from "node:child_process";
import { join } from "node:path";
import { create, fromBinary, toBinary, type MessageInitShape } from "@bufbuild/protobuf";
import type { APIRequestContext } from "@playwright/test";
import { test, expect } from "./trading-fixture";
import { brokerReady, liveAccount, livePolicy, rpc, strategies, trading } from "./rpc-fixture";
import { openView } from "./navigation-helper";
import {
  RequestSchema,
  ResponseSchema,
  RunState,
  type Response,
} from "../src/generated/asterion/lab_pb";
import { Period } from "../src/generated/asterion/common_pb";

type LabCall = NonNullable<MessageInitShape<typeof RequestSchema>["call"]>;
type LabReply<C> = Extract<Response["result"], { case: C }> extends { value: infer V } ? V : never;
async function lab<C extends NonNullable<LabCall["case"]>>(
  request: APIRequestContext,
  call: Extract<LabCall, { case: C }>,
): Promise<LabReply<C>> {
  const response = await request.post("/__asterion/lab", {
    headers: { "Content-Type": "application/octet-stream" },
    data: Buffer.from(toBinary(RequestSchema, create(RequestSchema, { id: 1n, call }))),
  });
  expect(response.ok()).toBe(true);
  const reply = fromBinary(ResponseSchema, await response.body());
  expect(
    reply.result.case,
    JSON.stringify(reply.result.case === "error" ? reply.result.value : null),
  ).toBe((call as { case: C }).case);
  return reply.result.value as LabReply<C>;
}

test.use({ strategyRuntime: true });

test(
  "deploy a successful backtest's frozen source and settings to the independent Trading host",
  { tag: ["@journey", "@trading"] },
  async ({ page, tradingHost }, testInfo) => {
    test.setTimeout(120000);
    const source = join(__dirname, "../../../..");
    execFileSync(join(source, "build/electron-resources/python/bin/python3"), [
      "-I",
      "-B",
      join(source, "tests/lab/warehouse_fixture.py"),
      join(source, "apps/services/lab"),
      join(process.env.ASTERION_TEST_WAREHOUSE_TLS!, "../../data"),
    ]);
    const { research_directory: repository } = await rpc(page.request, "runtime.snapshot");
    expect(repository).toBe(join(process.env.ASTERION_ROOT!, "research"));
    const original = (
      await lab(page.request, {
        case: "readSource",
        value: { repository, path: "strategies/ma_cross.py" },
      })
    ).content.replace("class MaCross(", "class DeploymentProbe(");
    const strategySource = { repository, path: "strategies/deployment_probe.py" };
    await lab(page.request, {
      case: "saveSource",
      value: { source: strategySource, content: original, create: true },
    });
    await page.goto("/");
    await openView(page, "research");
    await page.getByRole("button", { name: "设置回测条件", exact: true }).click();
    const scope = page.getByRole("dialog", { name: "回测条件" });
    await scope.getByRole("combobox", { name: "品种" }).fill("SHFE/rb");
    await scope.getByRole("combobox", { name: "品种" }).press("Escape");
    await scope.getByLabel("开始日").fill("2026-01-05");
    await scope.getByLabel("结束日").fill("2026-02-13");
    for (const label of ["单笔手数", "单合约持仓手数", "账户总持仓手数"])
      await scope.getByLabel(label, { exact: true }).fill("10");
    await scope.getByLabel("总权重上限", { exact: true }).fill("0.4");
    await scope.getByLabel("净权重上限", { exact: true }).fill("0.2");
    await scope.getByRole("button", { name: "完成", exact: true }).click();
    await page.getByRole("button", { name: "DeploymentProbe", exact: true }).click();
    const research = page.locator(".lab-research:visible");
    await research.getByLabel("fast").fill("2");
    await research.getByLabel("slow").fill("4");
    await research.getByRole("button", { name: "运行", exact: true }).click();
    await expect(page.getByRole("heading", { name: "月度收益" })).toBeVisible({ timeout: 30000 });
    const [run] = (
      await lab(page.request, {
        case: "listRuns",
        value: { strategy: "deployment_probe.DeploymentProbe" },
      })
    ).runs;
    expect(run.state).toBe(RunState.SUCCEEDED);
    await lab(page.request, {
      case: "saveSource",
      value: {
        source: strategySource,
        content: `${original}\nraise RuntimeError("edited after research")\n`,
      },
    });
    await page.getByRole("button", { name: "在回测中打开", exact: true }).click();
    await page.getByRole("button", { name: "去部署", exact: true }).click();
    const packages = page.getByRole("region", { name: "策略版本", exact: true });
    await expect(packages).toContainText(run.id);
    const prepared = page.waitForResponse(response => {
      if (!response.url().endsWith("/__asterion/lab")) return false;
      const bytes = response.request().postDataBuffer();
      return !!bytes && fromBinary(RequestSchema, bytes).call.case === "buildPackage";
    });
    await packages.getByRole("button", { name: "部署", exact: true }).click();
    const preparation = fromBinary(ResponseSchema, await (await prepared).body()).result;
    expect(preparation.case).toBe("buildPackage");
    if (preparation.case !== "buildPackage") throw new Error("Lab did not prepare the package");
    const code = preparation.value.files.find(file => file.path === strategySource.path);
    expect(code).toBeDefined();
    expect(new TextDecoder().decode(code!.content)).toBe(original);
    await expect(packages.getByRole("table", { name: "已部署的版本" })).toContainText(
      "strategies.deployment_probe:DeploymentProbe",
    );
    await expect(
      packages.getByRole("table", { name: "已部署的版本" }).getByRole("button", { name: run.id }),
    ).toBeInViewport({ ratio: 1 });
    expect(await packages.evaluate(node => node.scrollWidth <= node.clientWidth)).toBe(true);
    await tradingHost.restart();
    await packages.getByRole("button", { name: "刷新版本", exact: true }).click();
    await expect(packages.getByRole("table", { name: "已部署的版本" })).toContainText(
      "deployment_probe.DeploymentProbe",
    );
    const account = tradingHost.connection;
    const state = await strategies(page.request, account);
    expect(state.packages).toHaveLength(1);
    const published = state.packages[0];
    expect(published.research).toMatchObject({
      runId: run.id,
      symbols: ["SHFE/rb"],
      period: "PERIOD_DAY",
      datasetVersion: run.pinned!.datasetVersion,
      params: { fast: { integer: "2" }, slow: { integer: "4" } },
      weightLimits: { gross: { text: "0.4" }, net: { text: "0.2" } },
    });
    expect(state.instance).toBeUndefined();
    await trading(page.request, account, await livePolicy(page.request));
    await brokerReady(page.request, account);
    const form = page.getByRole("region", { name: "配置策略实例", exact: true });
    const configure = async (id: string) => {
      await form.getByLabel("实例 ID", { exact: true }).fill(id);
      await form
        .getByRole("combobox", { name: "已部署版本", exact: true })
        .selectOption(`${published.name}/${published.version}`);
      await expect(form.getByRole("combobox", { name: "K 线周期", exact: true })).toHaveValue(
        String(Period.DAY),
      );
      await expect(form.getByLabel("fast", { exact: true })).toHaveValue("2");
      await expect(form.getByLabel("slow", { exact: true })).toHaveValue("4");
      await expect(form.getByLabel("总绝对权重预算", { exact: true })).toHaveValue("0.4");
      await expect(form.getByLabel("净权重绝对值预算", { exact: true })).toHaveValue("0.2");
      await expect(form.getByLabel("SHFE.rb2610", { exact: true })).not.toBeChecked();
      await form.getByLabel("SHFE.rb2610", { exact: true }).check();
      await form.getByRole("button", { name: "核对启动配置", exact: true }).click();
    };
    await configure("ui-run-a");
    await page.screenshot({
      path: testInfo.outputPath("strategy-configuration.png"),
      fullPage: true,
    });
    expect((await strategies(page.request, account)).instance).toBeUndefined();
    await form.getByRole("button", { name: "确认启动", exact: true }).click();
    // The running instance leads the strategy view.
    const monitor = page.getByRole("region", { name: "策略运行状态", exact: true });
    await expect(monitor).toContainText("正在现场预热", { timeout: 20000 });
    // Targets and positions are in view without scrolling.
    await page.evaluate(() => window.scrollTo(0, 0));
    await expect(monitor.getByRole("table", { name: "目标与实际持仓" })).toBeInViewport();
    const started = (await strategies(page.request, account)).instance;
    expect(started.instance.id).toBe("ui-run-a");
    expect(started.instance).toMatchObject({
      period: "PERIOD_DAY",
      params: published.research.params,
    });
    expect((await liveAccount(page.request, account)).session.authorizedMs).toBeUndefined();
    await page.reload();
    await openView(page, "tradingStrategies");
    await expect(monitor).toContainText("ui-run-a");
    expect((await strategies(page.request, account)).instance.processId).toBe(started.processId);
    await page.screenshot({ path: testInfo.outputPath("strategy-instance.png"), fullPage: true });
    await monitor.getByRole("button", { name: "停止策略并撤单", exact: true }).click();
    await expect(monitor).toContainText("停止实例 ui-run-a");
    await monitor.getByRole("button", { name: "确认停止并撤单", exact: true }).click();
    await expect(monitor.getByRole("status")).toHaveText("已停止");
    await configure("ui-run-b");
    await form.getByRole("button", { name: "确认启动", exact: true }).click();
    await expect(monitor).toContainText("正在现场预热");
    await tradingHost.restart();
    // An orderly host shutdown records the stop before it exits; a crash is
    // recovered as interrupted (covered by the strategy process test).
    await expect(monitor.getByRole("status")).toHaveText("已停止", { timeout: 20000 });
    const stopped = (await strategies(page.request, account)).instance;
    expect(stopped.instance.id).toBe("ui-run-b");
    expect(stopped.processId).toBeUndefined();
  },
);
