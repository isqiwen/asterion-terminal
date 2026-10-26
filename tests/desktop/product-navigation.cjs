async function openView(page, view) {
  const destinations = {
    history: ["数据", "历史数据"],
    records: ["数据", "数据仓库"],
    backtest: ["回测"],
    market: ["行情", "市场"],
    watchlist: ["行情", "自选"],
    contract: ["行情", "合约"],
  };
  const [module, title] = destinations[view];
  await page
    .getByRole("navigation", { name: "业务工作区", exact: true })
    .getByRole("button", { name: module, exact: true })
    .click();
  if (title)
    await page
      .getByRole("navigation", { name: "子模块视图", exact: true })
      .getByRole("button", { name: title, exact: true })
      .click();
}
module.exports = { openView };
