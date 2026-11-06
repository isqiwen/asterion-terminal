const { openView, openMarketSection } = require("./product-navigation.cjs");
// Editors such as VS Code export this; Electron would then start as plain Node.
delete process.env.ELECTRON_RUN_AS_NODE;
// Exercise the unchanged production host with an isolated resource tree and explicit SDK fixture.
const { _electron: electron, expect } = require("@playwright/test");
const assert = require("node:assert/strict");
const { enterWorkbench, testNode } = require("./electron_startup.cjs");
const fs = require("node:fs/promises");
const os = require("node:os");
const path = require("node:path");
const { spawn } = require("node:child_process");
const { once } = require("node:events");
const { createInterface } = require("node:readline");
(async () => {
  const temp = await fs.mkdtemp(path.join(os.tmpdir(), "asterion-depth-"));
  let application, market;
  try {
    const client = path.join(temp, "apps/clients/terminal");
    await fs.mkdir(client, { recursive: true });
    await fs.cp(path.resolve("apps/clients/terminal/electron"), path.join(client, "electron"), {
      recursive: true,
    });
    await fs.symlink(path.resolve("apps/clients/terminal/dist"), path.join(client, "dist"), "dir");
    const build = path.join(temp, "build/Release");
    await fs.mkdir(build, { recursive: true });
    await fs.copyFile(
      path.resolve(process.env.ASTERION_CPP_BUILD || "build/Release", "product.json"),
      path.join(build, "product.json"),
    );
    const resources = path.join(temp, "build/electron-resources");
    await fs.mkdir(resources, { recursive: true });
    await fs.cp(path.resolve("build/electron-resources/crash"), path.join(resources, "crash"), {
      recursive: true,
    });
    const native = path.join(resources, "native");
    await fs.mkdir(native, { recursive: true });
    for (const item of await fs.readdir(path.resolve("build/electron-resources/native"))) {
      await fs.cp(path.resolve("build/electron-resources/native", item), path.join(native, item), {
        recursive: true,
      });
    }
    market = spawn(
      "python3",
      [
        "tests/support/market_installation.py",
        path.resolve(process.env.ASTERION_CPP_BUILD || "build/Release"),
        path.join(temp, "market"),
      ],
      { stdio: ["pipe", "pipe", "inherit"] },
    );
    const lines = createInterface({ input: market.stdout });
    const port = await Promise.race([
      once(lines, "line").then(([line]) => Number(line)),
      once(market, "exit").then(([code]) => {
        throw new Error(`Market fixture exited: ${code}`);
      }),
    ]);
    assert.ok(port > 0 && port <= 65535);
    const tls = path.join(temp, "market/config/tls");
    const clientTls = path.join(temp, "node/terminal/data/tls");
    await fs.mkdir(clientTls, { recursive: true });
    for (const extension of ["crt", "key"])
      await fs.copyFile(
        path.join(tls, `reader.${extension}`),
        path.join(clientTls, `client.${extension}`),
      );
    const fingerprint = new (require("node:crypto").X509Certificate)(
      await fs.readFile(path.join(tls, "server.crt")),
    ).fingerprint256
      .replaceAll(":", "")
      .toLowerCase();
    application = await electron.launch({
      recordVideo: process.env.ASTERION_UI_VIDEO_DIR
        ? {
            dir: path.join(process.env.ASTERION_UI_VIDEO_DIR, "electron_depth"),
            size: { width: 1440, height: 900 },
          }
        : undefined,
      chromiumSandbox: true,
      executablePath: require("../../scripts/desktop/electron-path.cjs")(),
      args: [path.join(client, "electron"), `--user-data-dir=${temp}/ui`],
      env: {
        ...process.env,
        ASTERION_ROOT: path.join(temp, "node"),
        ...testNode,
      },
      timeout: 30000,
    });
    const page = await application.firstWindow();
    await page.waitForFunction(() => !!window.asterionDesktop);
    try {
      // First launch in a fresh node: the task programs are verified and
      // deployed, then each service must answer its first heartbeat.
      await enterWorkbench(page);
    } catch (error) {
      console.error("Isolated startup UI:", await page.locator("body").innerText());
      const state = JSON.parse(
        await page.evaluate(() =>
          window.asterionDesktop.request(
            JSON.stringify({ version: 1, method: "runtime.snapshot", params: {} }),
          ),
        ),
      );
      for (const dir of [path.join(temp, "node/logs")])
        for (const file of await fs.readdir(dir).catch(() => []))
          console.error(
            "LOG",
            file,
            (await fs.readFile(path.join(dir, file), "utf8")).split("\n").slice(-8).join("\n"),
          );
      console.error(
        "Isolated services:",
        JSON.stringify(
          state.result?.nodes
            ?.flatMap(node => node.health?.services ?? [])
            .map(service => ({
              id: service.id,
              state: service.state,
              health: service.health,
              error: service.error,
            })),
        ),
      );
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
    await call("market.endpoint.save", { host: "127.0.0.1", port, fingerprint });
    await call("market.open");
    const snapshot = async () =>
      (await call("market.call", { request: { getBoard: {} } })).market.getBoard;
    await expect
      .poll(
        async () => {
          const state = await snapshot();
          // Shown when the wait fails: where the market service stopped.
          return (
            state.rows?.[0]?.quote?.bids?.[1]?.price?.text ??
            `feed=${state.feed?.state} catalog=${JSON.stringify(state.catalog?.status ?? {})}`
          );
        },
        { timeout: 15000 },
      )
      .toBe("3508.25");
    const full = await snapshot();
    assert.equal(full.catalog.status.state, "CATALOG_STATE_READY");
    assert.equal(full.catalog.contracts[0].name, "螺纹钢");
    assert.equal((await call("runtime.snapshot")).market_endpoint.instruments.length, 0);
    await openView(page, "market");
    await page.getByRole("tab", { name: "实时行情", exact: true }).click();
    await expect(page.getByRole("button", { name: "期货全景", exact: true })).toBeVisible();
    await expect(page.locator(".quote-select").filter({ hasText: "螺纹钢" })).toHaveCount(1);
    await expect(page.getByRole("cell", { name: "3510", exact: true })).toBeVisible();
    await fs.mkdir(path.resolve("build/native-depth-check"), { recursive: true });
    await page.screenshot({ path: path.resolve("build/native-depth-check/full-market.png") });
    await openMarketSection(page, "自选");
    await expect(page.locator(".market-quote-table table tbody tr")).toHaveCount(0);
    await call("market.watchlist", { instruments: [{ exchange: "SHFE", symbol: "rb2610" }] });
    const quote = full.rows[0].quote;
    assert.equal(quote.open.text, "3490.25");
    assert.equal(quote.openInterestChange.text, "-25.25");
    assert.equal(quote.previousClose.text, "3480.125");
    assert.equal(quote.upperLimit.text, "3800.5");
    assert.equal(quote.lowerLimit, undefined);
    assert.equal(quote.asks[2].lots, "0");
    assert.equal(quote.bids[2].price, undefined);
    assert.equal(quote.bids[2].lots, undefined);
    assert.equal(quote.bids[3].lots, undefined);
    assert.equal(quote.asks[4].price.text, "3515");
    await openMarketSection(page, "自选");
    const row = page.locator(".market-quote-table tbody tr");
    await expect(row).toHaveCount(1);
    await expect(row.getByRole("button", { name: "rb2610", exact: true })).toHaveText("螺纹钢");
    await expect(row.getByRole("cell", { name: quote.last.text, exact: true })).toBeVisible();
    await openView(page, "contract");
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
    await call("market.close");
    console.log(
      "Electron market: production host, isolated SDK, real market service and Node-API, full catalog with empty watchlist, Chinese names, exact depth, session prices and previous close passed",
    );
  } finally {
    try {
      if (application) await application.close();
    } finally {
      if (market) {
        assert.equal(market.exitCode, null, "Terminal must not stop Market");
        const closed = once(market, "exit");
        market.stdin.end();
        await closed;
      }
      // The local Lab exits with the application that started it.
      await new Promise(resolve => setTimeout(resolve, 2000));
      await fs.rm(temp, { recursive: true, force: true });
    }
  }
})().catch(error => {
  console.error(error);
  // A child left running by the failure would otherwise keep Node waiting forever.
  process.exit(1);
});
