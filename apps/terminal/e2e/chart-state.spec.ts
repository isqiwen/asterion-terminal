import { expect, test } from "@playwright/test";
import { nativeContext } from "./native";
test("chart zoom survives polling, docking, reload and native detach", async ({
  page,
  context,
}) => {
  await nativeContext(context);
  const user = {
    email: "chart@example.com",
    first_name: "Chart",
    last_name: "Test",
  };
  const bars = Array.from({ length: 100 }, (_, i) => ({
    contract: "SHFE.rb2610",
    event_time: new Date(Date.UTC(2026, 8, 14, 1, i)).toISOString(),
    open: "3200",
    high: "3250",
    low: "3190",
    close: String(3210 + (i % 20)),
    volume: "100",
  }));
  const snapshots = [
    {
      id: "snapshot-test",
      manifest: {
        contracts: ["SHFE.rb2610"],
        source: "synthetic test",
        rows: 100,
        start: bars[0].event_time,
        end: bars[99].event_time,
      },
    },
  ];
  await context.route("**/api/v1/**", (route) => {
    const path = new URL(route.request().url()).pathname;
    if (path.includes("/account/security")) return route.fallback();
    return route.fulfill({
      json: path.endsWith("/login")
        ? { session: "session", user }
        : path.endsWith("/me")
          ? user
          : path.endsWith("/health")
            ? { status: "ready" }
            : path.endsWith("/bars")
              ? bars
              : path.endsWith("/snapshots")
                ? snapshots
                : [],
    });
  });
  await page.goto("/");
  await page.getByLabel("邮箱", { exact: true }).fill(user.email);
  await page.getByLabel("密码", { exact: true }).fill("password-test-123");
  await page.getByRole("button", { name: "登录", exact: true }).click();
  const chart = page.locator(".chart");
  await expect(chart.locator("canvas").first()).toBeVisible();
  await chart.hover();
  await page.mouse.wheel(0, -300);
  await expect
    .poll(() =>
      page.evaluate(
        () => JSON.parse(localStorage.getItem("asterion.layout")!).viewport?.to,
      ),
    )
    .toBeGreaterThan(0);
  const before = JSON.parse((await chart.getAttribute("data-visible-range"))!);
  await page.getByLabel("行情面板停靠位置").selectOption("top");
  await expect(page.getByRole("separator")).toHaveAttribute(
    "aria-orientation",
    "horizontal",
  );
  const market = await page.locator(".market-board").boundingBox();
  const chartBox = await page.locator(".business-panel").boundingBox();
  expect(market!.y + market!.height).toBeLessThanOrEqual(chartBox!.y);
  await page.reload();
  await expect(page.getByLabel("行情面板停靠位置")).toHaveValue("top");
  await expect(chart).toHaveAttribute("data-visible-range", /.+/);
  await expect
    .poll(async () => {
      const r = JSON.parse((await chart.getAttribute("data-visible-range"))!);
      return r.to - r.from;
    })
    .toBeCloseTo(before.to - before.from, 0);
  const childPromise = context.waitForEvent("page");
  await page.getByRole("button", { name: "拆出图表", exact: true }).click();
  const child = await childPromise;
  await expect(child.locator(".chart")).toHaveAttribute(
    "data-visible-range",
    /.+/,
  );
  await expect
    .poll(async () => {
      const r = JSON.parse(
        (await child.locator(".chart").getAttribute("data-visible-range"))!,
      );
      return r.to - r.from;
    })
    .toBeCloseTo(before.to - before.from, 0);
  await page.getByRole("button", { name: "归并图表", exact: true }).click();
  await expect(chart).toBeVisible();
  await page.getByRole("button", { name: "锁定终端", exact: true }).click();
  await expect(
    page.getByRole("heading", { name: "终端已锁定", exact: true }),
  ).toBeVisible();
  await page.getByLabel("PIN", { exact: true }).fill("246810");
  await page.getByRole("button", { name: "解锁", exact: true }).click();
  await expect(chart).toBeVisible();
  await expect
    .poll(async () => {
      const r = JSON.parse((await chart.getAttribute("data-visible-range"))!);
      return r.to - r.from;
    })
    .toBeCloseTo(before.to - before.from, 0);
  await page.screenshot({ path: "../../.state/docked-chart.png" });
});
