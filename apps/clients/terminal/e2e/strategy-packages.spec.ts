import { test, expect } from "./trading-fixture";
import { liveAccount, livePolicy, rpc, trading } from "./rpc-fixture";
import { openView } from "./navigation-helper";

test.use({ strategyRuntime: true });

test(
  "publish, configure, start, observe and stop a strategy on the independent Trading host",
  { tag: ["@journey", "@trading"] },
  async ({ page, tradingHost }, testInfo) => {
    test.setTimeout(90000);
    await page.goto("/");
    await openView(page, "trading");
    const packages = page.getByRole("region", { name: "策略包", exact: true });
    await packages
      .getByRole("combobox", { name: "策略", exact: true })
      .selectOption("ma_cross.MaCross");
    await packages.getByRole("button", { name: "准备策略包", exact: true }).click();
    await expect(packages.getByText(/^ma_cross\.MaCross · \d{8}-\d{6}$/)).toBeVisible();
    await expect(packages.getByText(/strategies\/ma_cross\.py/)).toBeVisible();
    await packages.getByRole("button", { name: "发布到此交易主机", exact: true }).click();
    await expect(packages.getByRole("listitem")).toContainText("strategies.ma_cross:MaCross");
    await expect(
      packages.getByRole("button", { name: "发布到此交易主机", exact: true }),
    ).toHaveCount(0);
    await tradingHost.restart();
    await packages.getByRole("button", { name: "读取主机策略包", exact: true }).click();
    await expect(packages.getByRole("listitem")).toContainText("ma_cross.MaCross");
    const state = await rpc(page.request, "trading.call", {
      account: tradingHost.connection,
      request: { strategy: { inspect: {} } },
    });
    expect(state.trading.strategy.packages).toHaveLength(1);
    const published = state.trading.strategy.packages[0];
    expect(state.trading.strategy.instance).toBeUndefined();
    const account = tradingHost.connection;
    await trading(page.request, account, await livePolicy(page.request));
    await expect
      .poll(async () => (await liveAccount(page.request, account)).phase, { timeout: 20000 })
      .toBe("ready");
    const form = packages.getByRole("region", { name: "配置策略实例", exact: true });
    const configure = async (id: string) => {
      await form.getByLabel("实例 ID", { exact: true }).fill(id);
      await form
        .getByRole("combobox", { name: "已发布版本", exact: true })
        .selectOption(`${published.name}/${published.version}`);
      await form.getByLabel("SHFE/rb2610", { exact: true }).check();
      await form.getByRole("button", { name: "核对启动配置", exact: true }).click();
    };
    await configure("ui-run-a");
    await page.screenshot({
      path: testInfo.outputPath("strategy-configuration.png"),
      fullPage: true,
    });
    expect(
      (
        await rpc(page.request, "trading.call", {
          account,
          request: { strategy: { inspect: {} } },
        })
      ).trading.strategy.instance,
    ).toBeUndefined();
    await form.getByRole("button", { name: "确认启动", exact: true }).click();
    const monitor = packages.getByRole("region", { name: "策略运行状态", exact: true });
    await expect(monitor).toContainText("正在现场预热", { timeout: 20000 });
    const started = (
      await rpc(page.request, "trading.call", { account, request: { strategy: { inspect: {} } } })
    ).trading.strategy.instance;
    expect(started.configuration.id).toBe("ui-run-a");
    expect((await liveAccount(page.request, account)).authorization).toBeUndefined();
    await page.reload();
    await openView(page, "trading");
    await expect(monitor).toContainText("ui-run-a");
    expect(
      (
        await rpc(page.request, "trading.call", {
          account,
          request: { strategy: { inspect: {} } },
        })
      ).trading.strategy.instance.processId,
    ).toBe(started.processId);
    await page.screenshot({ path: testInfo.outputPath("strategy-instance.png"), fullPage: true });
    await monitor.getByRole("button", { name: "停止策略并撤单", exact: true }).click();
    await expect(monitor).toContainText("确认停止实例 ui-run-a");
    await monitor.getByRole("button", { name: "确认停止并撤单", exact: true }).click();
    await expect(monitor.getByRole("status")).toHaveText("已停止");
    await configure("ui-run-b");
    await form.getByRole("button", { name: "确认启动", exact: true }).click();
    await expect(monitor).toContainText("正在现场预热");
    await tradingHost.restart();
    // An orderly host shutdown records the stop before it exits; a crash is
    // recovered as interrupted (covered by the strategy process test).
    await expect(monitor.getByRole("status")).toHaveText("已停止", { timeout: 20000 });
    const stopped = (
      await rpc(page.request, "trading.call", { account, request: { strategy: { inspect: {} } } })
    ).trading.strategy.instance;
    expect(stopped.configuration.id).toBe("ui-run-b");
    expect(stopped.processId).toBeUndefined();
  },
);
