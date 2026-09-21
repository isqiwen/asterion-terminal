import { coverageIdentity } from "./reference-fixture";
import { createHash } from "node:crypto";
import { expect, type Page, type Route } from "@playwright/test";

export const timeVersion = {
  id: "b".repeat(64), spec: {
    schema_version: 1, exchange: "SHFE", product: "RB", title: "测试交易时间", timezone: "Asia/Shanghai", calendar_source: "测试", night_source: "测试日盘",
    calendar: Array.from({length:367}, (_,i) => ({date:new Date(Date.UTC(2023,11,31+i)).toISOString().slice(0,10), is_open:true, night_open:false})),
    periods:[{start:"2024-01-01",end:"2024-12-31",source:"测试",day:[{start:"09:00:00",end:"15:00:00",end_offset:0,phase:"continuous"}],night:[]}], exceptions:[],
  },
};
export const spec = {
  trading_time: timeVersion,
  basis: null,
  contract: coverageIdentity().catalog.contracts[0],
  title: "测试规则",
  source: "离线测试",
  multiplier: "10",
  tick_size: "1",
  periods: [
    {
      start: "2024-01-01",
      end: "2024-12-31",
      margin_rate: "0.1",
      fee_mode: "per_lot",
      open_fee: "2",
      close_fee: "2",
      settlement_basis: null,
    },
  ],
};
export const rules = { id: "a".repeat(64), spec };
export function rulesRoute(route: Route) {
  return route.fulfill({
    json:
      route.request().method() === "GET"
        ? [rules]
        : {
            id: createHash("sha256")
              .update(route.request().postData() || "")
              .digest("hex"),
            spec: route.request().postDataJSON(),
          },
  });
}
export async function selectRules(page: Page) {
  await page
    .getByRole("combobox", { name: "固定规则版本", exact: true })
    .selectOption(rules.id);
  await expect(page.getByLabel("固定合约规则")).toContainText("乘数 10");
}
