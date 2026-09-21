import { expect, test } from "@playwright/test";
for (const rows of ["normal", "many"]) test(`dashboard ${rows} rows preserve composition and bounded scrolling`, async ({page}) => {
  await page.goto(`/e2e/fixtures/dashboard.html?rows=${rows}`);
  const market=page.getByRole("region",{name:"实时行情",exact:true});
  const account=page.getByRole("region",{name:"账户与持仓",exact:true});
  const risk=page.getByRole("region",{name:"风险概览",exact:true});
  await expect(market).toBeVisible();
  for (const width of [1440,1280]) {
    await page.setViewportSize({width,height:900});
    const [m,a,r]=await Promise.all([market.boundingBox(),account.boundingBox(),risk.boundingBox()]);
    expect(m!.x+m!.width).toBeLessThan(a!.x);
    expect(m!.y).toBe(a!.y);
    expect(m!.width/a!.width).toBeCloseTo(1.5,1);
    expect(r!.x).toBe(a!.x);
    expect(r!.y).toBeGreaterThanOrEqual(a!.y+a!.height+19);
  }
  for (const label of ["实时报价滚动区","持仓明细滚动区"]) {
    const scroll=page.getByRole("region",{name:label,exact:true});
    expect(await scroll.evaluate(el=>el.scrollHeight>el.clientHeight)).toBe(rows==="many");
    await scroll.focus();
    if(rows==="many") { await page.keyboard.press("End"); await expect.poll(()=>scroll.evaluate(el=>el.scrollTop)).toBeGreaterThan(0); }
  }
  expect(await page.locator("td.numeric").first().evaluate(el=>getComputedStyle(el).textAlign)).toBe("right");
  await page.screenshot({path:`../../.state/dashboard-layout-${rows}.png`});
  await page.setViewportSize({width:900,height:900});
  const [m,a]=await Promise.all([market.boundingBox(),account.boundingBox()]);
  expect(a!.x).toBe(m!.x);
  expect(a!.y).toBeGreaterThan(m!.y+m!.height);
  expect(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth)).toBe(true);
});

test("column changes save, reload and cancel without dropping widgets", async ({page}) => {
  await page.goto("/e2e/fixtures/dashboard.html");
  await page.getByRole("button",{name:"编辑布局",exact:true}).click();
  await page.getByLabel("风险概览位置",{exact:true}).selectOption("primary");
  await page.getByRole("button",{name:/保存布局/}).click();
  await page.reload();
  await expect(page.locator(".dashboard-primary").getByRole("region",{name:"风险概览",exact:true})).toBeVisible();
  await page.getByRole("button",{name:"编辑布局",exact:true}).click();
  await page.getByLabel("风险概览位置",{exact:true}).selectOption("secondary");
  await page.getByRole("button",{name:"取消编辑",exact:true}).click();
  await expect(page.locator(".dashboard-primary").getByRole("region",{name:"风险概览",exact:true})).toBeVisible();
});
