// Editors such as VS Code export this; Electron would then start as plain Node.
delete process.env.ELECTRON_RUN_AS_NODE;
// Exercise the unchanged production host with an isolated resource tree and explicit SDK fixture.
const { _electron: electron, expect } = require("@playwright/test");
const assert = require("node:assert/strict");
const fs = require("node:fs/promises");
const os = require("node:os");
const path = require("node:path");
(async () => {
  assert.equal(process.platform, "darwin", "This acceptance target is macOS Terminal only");
  const temp = await fs.mkdtemp(path.join(os.tmpdir(), "asterion-depth-"));
  let application;
  try {
    const client = path.join(temp, "apps/clients/terminal");
    await fs.mkdir(client, { recursive: true });
    await fs.cp(path.resolve("apps/clients/terminal/electron"), path.join(client, "electron"), {
      recursive: true,
    });
    await fs.symlink(path.resolve("apps/clients/terminal/dist"), path.join(client, "dist"), "dir");
    const resources = path.join(temp, "build/electron-resources");
    const native = path.join(resources, "native");
    await fs.mkdir(native, { recursive: true });
    for (const item of await fs.readdir(path.resolve("build/electron-resources/native"))) {
      if (["ctp-md.dylib", "ctp-trader.dylib"].includes(item)) continue;
      await fs.cp(path.resolve("build/electron-resources/native", item), path.join(native, item), {
        recursive: true,
      });
    }
    await fs.copyFile(
      path.resolve(process.env.ASTERION_CPP_BUILD || "build/Release", "libasterion_test_ctp.dylib"),
      path.join(native, "ctp-md.dylib"),
    );
    await fs.copyFile(
      path.resolve(
        process.env.ASTERION_CPP_BUILD || "build/Release",
        "libasterion_test_ctp_trader.dylib",
      ),
      path.join(native, "ctp-trader.dylib"),
    );
    await fs.symlink(
      path.resolve("build/electron-resources/remote-linux"),
      path.join(resources, "remote-linux"),
      "dir",
    );
    application = await electron.launch({
      chromiumSandbox: true,
      executablePath: require("electron"),
      args: [path.join(client, "electron"), `--user-data-dir=${temp}/ui`],
      env: {
        ...process.env,
        ASTERION_NODE_DIRECTORY: path.join(temp, "node"),
        ASTERION_TEST_NODE_ISOLATED: "1",
      },
      timeout: 30000,
    });
    const page = await application.firstWindow();
    await page.waitForFunction(() => !!window.asterionDesktop);
    try {
      await page.getByRole("button", { name: "进入工作台", exact: true }).click({ timeout: 15000 });
    } catch (error) {
      console.error("Isolated startup UI:", await page.locator("body").innerText());
      throw error;
    }
    const call = async (method, params = {}) => {
      const response = await page.evaluate(
        async ({ method, params }) =>
          JSON.parse(
            await window.asterionDesktop.request(JSON.stringify({ version: 1, method, params })),
          ),
        { method, params },
      );
      assert.equal(response.error, undefined, JSON.stringify(response.error));
      return response.result;
    };
    await call("market.local");
    await call("market.connect", {
      front: "tcp://127.0.0.1:1",
      broker: "test",
      user: "fixture",
      password: "explicit-depth-fixture",
      instruments: [],
    });
    await call("market.catalog", {
      front: "tcp://127.0.0.1:1",
      broker: "test",
      user: "catalog",
      password: "explicit-catalog-fixture",
      app_id: "",
      auth_code: "",
    });
    await expect
      .poll(
        async () =>
          (await call("runtime.snapshot")).market?.subscriptions[0]?.quote?.bid_levels[0]?.price,
        { timeout: 15000 },
      )
      .toBe("3508.25");
    const fullMarket = (await call("runtime.snapshot")).market;
    assert.equal(fullMarket.catalog.phase, "ready");
    assert.equal(fullMarket.catalog.contracts[0].name, "螺纹钢");
    assert.equal(fullMarket.watchlist.length, 0);
    const navigation = page.getByRole("navigation", { name: "业务工作区" });
    await navigation.getByRole("button", { name: "市场", exact: true }).click();
    await page.getByRole("tab", { name: "实时行情", exact: true }).click();
    await expect(page.getByRole("button", { name: "期货全景", exact: true })).toBeVisible();
    await expect(page.locator(".quote-select").filter({ hasText: "螺纹钢" })).toHaveCount(1);
    await expect(page.getByRole("cell", { name: "3510", exact: true })).toBeVisible();
    await fs.mkdir(path.resolve("build/native-depth-check"), { recursive: true });
    await page.screenshot({ path: path.resolve("build/native-depth-check/full-market.png") });
    await navigation.getByRole("button", { name: "自选", exact: true }).click();
    await expect(page.locator(".watchlist-table tbody tr")).toHaveCount(0);
    await call("market.subscribe", { instruments: [{ venue: "SHFE", symbol: "rb2610" }] });
    const quote = fullMarket.subscriptions[0].quote;
    assert.equal(quote.open, "3490.25");
    assert.equal(quote.open_interest_change, "-25.25");
    assert.equal(quote.previous_close, "3480.125");
    assert.equal(quote.upper_limit, "3800.5");
    assert.equal(quote.lower_limit, null);
    assert.equal(quote.ask_levels[1].quantity, 0);
    assert.equal(quote.bid_levels[1].price, null);
    assert.equal(quote.bid_levels[1].quantity, null);
    assert.equal(quote.bid_levels[2].quantity, null);
    assert.equal(quote.ask_levels[3].price, "3515");
    await page
      .getByRole("navigation", { name: "业务工作区" })
      .getByRole("button", { name: "自选", exact: true })
      .click();
    const previousClose = page
      .locator(".watchlist-table tbody tr")
      .first()
      .locator("td")
      .nth(20)
      .locator("span");
    await expect(previousClose).toHaveText("3480.125");
    await expect(previousClose).toHaveAttribute("title", "3480.125");
    await page
      .getByRole("navigation", { name: "业务工作区" })
      .getByRole("button", { name: "合约", exact: true })
      .click();
    const summary = page.locator(".contract-summary dl");
    for (const [label, value] of [
      ["开盘", "3490.25"],
      ["日增仓", "-25.25"],
      ["涨停", "3800.5"],
      ["跌停", "—"],
    ])
      await expect(
        summary
          .locator("div")
          .filter({ has: page.locator("dt", { hasText: new RegExp(`^${label}$`) }) })
          .locator("dd"),
      ).toHaveText(value);
    const depth = page.locator(".contract-depth");
    await expect(depth.locator(".depth-level").nth(1)).toContainText("3508.25");
    await expect(depth.locator(".depth-level").nth(2).locator("span").last()).toHaveText("0");
    await expect(depth.locator(".depth-level").nth(3).locator("span").first()).toHaveText("—");
    await expect(depth.locator(".depth-level").nth(4)).toContainText("3515");
    await expect(depth).not.toContainText("当前报价未提供更多档位");
    await fs.mkdir(path.resolve("build/native-depth-check"), { recursive: true });
    await page.screenshot({ path: path.resolve("build/native-depth-check/native-depth.png") });
    await call("market.disconnect");
    console.log(
      "Electron market: production host, isolated SDK, real Agent/market service and Node-API, full catalog with empty watchlist, Chinese names, exact depth, session prices and previous close passed",
    );
  } finally {
    try {
      if (application) await application.close();
    } finally {
      try {
        process.kill(
          Number(await fs.readFile(path.join(temp, "node/agent.pid"), "utf8")),
          "SIGTERM",
        );
        await new Promise(resolve => setTimeout(resolve, 2000));
      } catch (error) {
        if (!["ENOENT", "ESRCH"].includes(error.code)) throw error;
      }
      await fs.rm(temp, { recursive: true, force: true });
    }
  }
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
