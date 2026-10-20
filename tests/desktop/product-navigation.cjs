async function openView(page, view) {
  const destinations = {
    history: ["数据", "接入与采集"],
    records: ["数据", "管理与服务", "历史数据仓库"],
    backtest: ["回测", "实验管理"],
    market: ["实盘", "准备与运行", "市场"],
    watchlist: ["实盘", "准备与运行", "自选"],
    contract: ["实盘", "准备与运行", "合约"],
  };
  const [module, section, title] = destinations[view];
  await page
    .getByRole("navigation", { name: "业务工作区", exact: true })
    .getByRole("button", { name: module, exact: true })
    .click();
  await page
    .getByRole("navigation", { name: "业务子模块", exact: true })
    .getByRole("button", { name: section, exact: true })
    .click();
  if (title)
    await page
      .getByRole("navigation", { name: "子模块视图", exact: true })
      .getByRole("button", { name: title, exact: true })
      .click();
}
module.exports = { openView };
