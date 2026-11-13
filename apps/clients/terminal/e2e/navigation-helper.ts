import type { Page } from "@playwright/test";
import host from "../src/i18n/locales/en-US.json";
import market from "../features/futures-market/locales/en-US.json";
import data from "../features/data-workbench/locales/en-US.json";
import research from "../features/research/locales/en-US.json";
import trading from "../features/trading/locales/en-US.json";

// Exercise the public product navigation; do not inject internal route state.
const destinations = {
  market: ["行情"],
  research: ["研究", "策略"],
  factors: ["研究", "因子"],
  trading: ["交易"],
  overview: ["数据", "概览"],
  coverage: ["数据", "覆盖"],
  browse: ["数据", "浏览"],
} as const;

export async function openView(page: Page, view: keyof typeof destinations) {
  const [module, title] = destinations[view] as readonly string[];
  const english = (await page.locator("html").getAttribute("lang")) === "en-US";
  const messages: Record<string, string> = {
    ...host,
    ...market,
    ...data,
    ...research,
    ...trading,
  };
  const name = (key: string) => (english ? messages[key] : key);
  await page
    .getByRole("navigation", { name: name("业务工作区"), exact: true })
    .getByRole("button", { name: name(module), exact: true })
    .click();
  if (title)
    await page
      .getByRole("navigation", { name: name("子模块视图"), exact: true })
      .getByRole("button", { name: name(title), exact: true })
      .click();
}
