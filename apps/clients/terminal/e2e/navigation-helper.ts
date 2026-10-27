import type { Page } from "@playwright/test";
import host from "../src/i18n/locales/en-US.json";
import market from "../plugins/futures-market/locales/en-US.json";
import data from "../plugins/data-workbench/locales/en-US.json";
import research from "../plugins/research/locales/en-US.json";
import trading from "../plugins/trading/locales/en-US.json";

// Exercise the public product navigation; do not inject internal route state.
const destinations = {
  logic: ["策略", "规则"],
  strategies: ["策略", "策略包"],
  samples: ["分析", "样本"],
  statistics: ["分析", "统计"],
  history: ["数据", "历史数据"],
  materials: ["数据", "资料"],
  tables: ["数据", "全市场数据"],
  records: ["数据", "数据仓库"],
  factor: ["分析", "因子"],
  variables: ["分析", "变量"],
  backtest: ["回测"],
  performance: ["评估", "绩效"],
  risk: ["评估", "风险"],
  validation: ["评估", "回测比较"],
  market: ["行情", "市场"],
  watchlist: ["行情", "自选"],
  contract: ["行情", "合约"],
  trading: ["实盘", "交易"],
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
