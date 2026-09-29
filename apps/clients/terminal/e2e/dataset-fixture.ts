import { expect, type Page } from "@playwright/test";
// Test-only initialization for existing tick research/ledger coverage. It is not
// a client workflow or a replacement source. Production UI has no file importer.
export async function seedTickFixture(page: Page, path: string, publish = false) {
  async function call(method: string, params: Record<string, unknown>) {
    const response = await page.request.post("/__asterion/api", {
      data: { version: 1, method, params },
    });
    expect(response.ok()).toBe(true);
    expect((await response.json()).error).toBeUndefined();
  }
  await call("futures.inspect_csv", {
    path,
    venue: "SHFE",
    symbol: "rb2610",
    product: "rb",
    delivery_month: "2026-10",
    currency: "CNY",
    price_increment: "1",
    quantity_increment: "1",
    multiplier: "10",
  });
  if (publish) await call("research.data.submit", { id: `fixture-${Date.now()}` });
  await page.reload();
  await page.getByRole("button", { name: "数据", exact: true }).click();
  await page.getByRole("button", { name: "数据存档与结算表", exact: true }).click();
}
