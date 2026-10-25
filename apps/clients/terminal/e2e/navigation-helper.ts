import type { Page } from "@playwright/test";
import host from "../src/i18n/locales/en-US.json";
import market from "../plugins/futures-market/locales/en-US.json";
import data from "../plugins/data-workbench/locales/en-US.json";
import research from "../plugins/research/locales/en-US.json";

// Exercise the public product navigation; do not inject internal route state.
const destinations = {
  logic: ["策略", "策略逻辑", "策略逻辑与事先样例"],
  strategies: ["策略", "策略管理与交付"],
  projects: ["分析", "数据探索", "研究项目与假设"],
  samples: ["分析", "数据探索", "独立研究样本"],
  statistics: ["分析", "数据探索", "探索统计"],
  history: ["数据", "接入与采集"],
  materials: ["数据", "接入与采集", "情报材料"],
  tables: ["数据", "管理与服务", "全市场数据"],
  records: ["数据", "管理与服务", "历史数据仓库"],
  factor: ["分析", "特征与因子"],
  variables: ["分析", "特征与因子", "研究变量定义"],
  backtest: ["回测", "实验管理"],
  performance: ["评估", "绩效与归因"],
  risk: ["评估", "风险与压力"],
  validation: ["评估", "稳健性与综合评价"],
  market: ["行情", "行情与自选", "市场"],
  watchlist: ["行情", "行情与自选", "自选"],
  contract: ["行情", "行情与自选", "合约"],
  trading: ["实盘", "订单与成交"],
} as const;

export async function openView(page: Page, view: keyof typeof destinations) {
  const [module, section, title] = destinations[view] as readonly string[];
  const english = (await page.locator("html").getAttribute("lang")) === "en-US";
  const messages: Record<string, string> = {
    ...host,
    ...market,
    ...data,
    ...research,
  };
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
