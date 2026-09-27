import { test, expect } from "@playwright/test";
import { mkdtemp, writeFile, rm, unlink } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";

test("published data remains usable for research after source deletion", async ({page}) => {
  const folder=await mkdtemp(join(tmpdir(),"asterion-published-ui-"));
  const csv=join(folder,"published.csv");
  const start=1790298000000000000n;
  await writeFile(csv,"timestamp_ns,price,quantity\n"+Array.from({length:40},(_,i)=>`${start+BigInt(i)*1000000000n},${100+i+i%3},1`).join("\n")+"\n");
  try {
    await page.goto("/");
    await page.getByRole("button",{name:"数据",exact:true}).click();
    if(await page.locator("details.data-import").getAttribute("open")===null) await page.getByText("导入 CSV",{exact:true}).click();
    for(const [label,value] of [["CSV 文件路径",csv],["品种代码","rb"],["实际合约","rb2610"],["交割月份","2026-10"],["价格步长","1"],["每手乘数","10"]])await page.getByLabel(label,{exact:true}).fill(value);
    await page.getByRole("button",{name:"校验并预览",exact:true}).click();
    await expect(page.getByRole("button",{name:"发布数据版本",exact:true})).toBeEnabled();
    await page.getByRole("button",{name:"发布数据版本",exact:true}).click();
    await expect(page.getByText("发布任务已提交，可关闭窗口。",{exact:true})).toBeVisible();
    await unlink(csv);
    await page.reload();
    await page.getByRole("button",{name:"数据",exact:true}).click();
    const publications=page.getByRole("region",{name:"数据发布",exact:true});
    await expect(publications.getByText("published.csv",{exact:true})).toBeVisible();
    await expect(publications.getByText("已发布",{exact:true})).toBeVisible({timeout:20000});
    await publications.getByRole("button",{name:"使用此版本",exact:true}).click();
    await expect(page.getByText("已发布数据",{exact:true})).toBeVisible();
    await expect(page.getByRole("button",{name:"发布数据版本",exact:true})).toHaveCount(0);
    await page.screenshot({path:join(__dirname,"../test-results/data-publication.png"),fullPage:true});
    await page.setViewportSize({width:800,height:900});
    expect(await page.evaluate(()=>document.documentElement.scrollWidth<=window.innerWidth)).toBe(true);
    await page.getByRole("button",{name:"开始研究",exact:true}).click();
    const research=page.getByRole("region",{name:"期货研究",exact:true});
    await research.getByRole("button",{name:"因子分析",exact:true}).click();
    await research.getByLabel("回看笔数",{exact:true}).fill("2");
    await research.getByLabel("未来收益笔数",{exact:true}).fill("1");
    await research.getByRole("button",{name:"开始分析",exact:true}).click();
    const row=research.getByRole("row").filter({hasText:"动量因子"}).last();
    await expect(row.getByText("已完成",{exact:true})).toBeVisible({timeout:20000});
    await row.getByRole("button",{name:"查看结果",exact:true}).click();
    await expect(research.getByRole("region",{name:"因子结果",exact:true}).getByText("37",{exact:true})).toBeVisible();
    await expect(research.getByText("发布中",{exact:true})).toHaveCount(0);
  } finally {await rm(folder,{recursive:true,force:true});}
});
