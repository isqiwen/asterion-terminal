import { expect, test } from "@playwright/test";
import { nativeContext } from "./native";

test("business directory selects exact contracts, role mappings and recovers errors", async ({context, page}) => {
  await nativeContext(context);
  const user = {email:"directory@example.com",first_name:"Data",last_name:"Test"};
  const contract = "SHFE.RB.202610.20251001";
  const branch = ["SHFE", "RB", "contracts", contract, "futures.daily", "fixture", "STANDARD"];
  const labels = ["SHFE · 上期所", "RB", "实际合约", contract, "日线", "fixture", "标准"];
  const nodes = branch.map((_, i) => ({path:branch.slice(0,i+1),label:labels[i],count:1}));
  nodes.push({path:[...branch.slice(0,-1), "RAW"],label:"原始",count:1});
  let fail = true;
  const queries: URLSearchParams[] = [];
  const roleQueries: URLSearchParams[] = [];
  await context.route("**/api/v1/**", async route => {
    const url = new URL(route.request().url()), path = url.pathname;
    if (path.includes("/account/security")) return route.fallback();
    if (path.endsWith("/access/scopes")) return route.fulfill({json:{token:"directory-scope",expires:Date.now()/1000+300}});
    if (path.endsWith("/data/hierarchy")) return fail ? route.fulfill({status:503,json:{detail:"目录暂不可用"}}) : route.fulfill({json:nodes});
    if (path.endsWith("/contract-roles/hierarchy")) return route.fulfill({json:[{product_id:"SHFE.RB",role:"main",count:1}]});
    if (path.endsWith("/data/catalog")) { queries.push(url.searchParams); return route.fulfill({json:{items:[],total:0,offset:0}}); }
    if (path.endsWith("/contract-roles/computed")) { roleQueries.push(url.searchParams); return route.fulfill({json:[]}); }
    return route.fulfill({json:path.endsWith("/login")?{session:"session",user}:path.endsWith("/me")?user:path.endsWith("/health")?{status:"ready"}:[]});
  });
  await page.goto("/");
  await page.getByLabel("邮箱",{exact:true}).fill(user.email);
  await page.getByLabel("密码",{exact:true}).fill("directory-password");
  await page.getByRole("button",{name:"登录",exact:true}).click();
  await page.getByRole("navigation",{name:"业务工作区"}).getByRole("button",{name:"数据",exact:true}).click();
  await page.getByRole("button",{name:"数据集",exact:true}).click();
  const directory = page.getByRole("complementary",{name:"期货数据目录"});
  await expect(directory.getByRole("alert")).toContainText("目录暂不可用");
  fail = false;
  await directory.getByRole("button",{name:"刷新目录"}).click();
  await expect(directory.getByRole("alert")).toHaveCount(0);
  for (const name of labels.slice(0,5)) await directory.getByRole("button",{name,exact:true}).click();
  await expect.poll(() => queries.at(-1)?.get("directory")).toBe(branch.slice(0,5).join("/"));
  await expect(page.getByRole("navigation",{name:"数据位置"})).toContainText(contract);
  await expect(directory.getByRole("button",{name:"连续序列",exact:true})).toHaveCount(0);
  await directory.getByRole("button",{name:"fixture",exact:true}).click();
  await directory.getByRole("button",{name:"原始",exact:true}).click();
  await expect.poll(() => queries.at(-1)?.get("layer")).toBe("RAW");
  await expect(page.getByLabel("加工阶段")).toHaveValue("RAW");
  await page.getByLabel("显示归档版本").check();
  await expect.poll(() => queries.at(-1)?.get("include_archived")).toBe("true");
  await directory.getByRole("button",{name:"角色映射",exact:true}).click();
  await directory.getByRole("button",{name:"主力",exact:true}).click();
  await expect(page.getByRole("region",{name:"合约角色诊断"})).toBeVisible();
  await expect.poll(() => roleQueries.at(-1)?.get("product_id")).toBe("SHFE.RB");
  await directory.getByRole("button",{name:"全部数据",exact:true}).click();
  await expect.poll(() => queries.at(-1)?.get("directory")).toBe("");
  await expect(page.getByLabel("显示归档版本")).toBeChecked();
  await page.screenshot({path:"../../.state/data-hierarchy.png",fullPage:true});
});
