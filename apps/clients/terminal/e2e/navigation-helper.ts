import type { Page } from "@playwright/test";
import host from "../src/i18n/locales/en-US.json";
import market from "../plugins/futures-market/locales/en-US.json";
import data from "../plugins/data-workbench/locales/en-US.json";

// Exercise the public product navigation; do not inject internal route state.
const destinations = {
  history: ["数据", "接入与采集"],
  tables: ["数据", "管理与服务", "全市场数据"],
  records: ["数据", "管理与服务", "历史数据仓库"],
  factor: ["分析", "特征与因子"],
  backtest: ["回测", "实验管理"],
  performance: ["评估", "绩效与归因"],
  market: ["实盘", "准备与运行", "市场"],
  watchlist: ["实盘", "准备与运行", "自选"],
  contract: ["实盘", "准备与运行", "合约"],
  trading: ["实盘", "订单与成交"],
} as const;

export async function openView(page: Page, view: keyof typeof destinations) {
  const [module, section, title] = destinations[view] as readonly string[];
  const english = (await page.locator("html").getAttribute("lang")) === "en-US";
  const messages: Record<string, string> = { ...host, ...market, ...data };
  const name = (key: string) => (english ? messages[key] : key);
  await page
    .getByRole("navigation", { name: name("业务工作区"), exact: true })
    .getByRole("button", { name: name(module), exact: true })
    .click();
  await page
    .getByRole("navigation", { name: name("业务子模块"), exact: true })
    .getByRole("button", { name: name(section), exact: true })
    .click();
  if (title)
    await page
      .getByRole("navigation", { name: name("子模块视图"), exact: true })
      .getByRole("button", { name: name(title), exact: true })
      .click();
}
