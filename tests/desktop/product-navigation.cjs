async function openView(page, view) {
  const destinations = {
    research: ["研究"],
    coverage: ["数据"],
    market: ["行情", "市场"],
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
async function openMarketSection(page, section) {
  await openView(page, "market");
  await page.getByRole("tab", { name: "实时行情", exact: true }).click();
  await page
    .getByRole("navigation", { name: "期货分类", exact: true })
    .getByRole("button", { name: section, exact: true })
    .click();
}
module.exports = { openView, openMarketSection };
