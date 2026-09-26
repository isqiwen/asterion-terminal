import { expect, test } from "@playwright/test";
import { nativeContext } from "./native";

test("first role calculation explicitly freezes inputs and retries the same publication", async ({ context, page }) => {
  await nativeContext(context);
  const user = { email: "role-start@example.com", first_name: "Role", last_name: "Test" };
  const ids = ["SHFE.AU.202506.20240617", "SHFE.AU.202508.20240617"];
  const symbols = ["AU2506", "AU2508"];
  const time = { id: "fixed-time", spec: { exchange: "SHFE", product: "AU", title: "测试时段与日历", timezone: "Asia/Shanghai" } };
  let candidate: any, spec: any;
  let saved: any;
  let failOptions = true;
  const publications: any[] = [];
  const requests: any[] = [];
  await context.route("**/api/v1/**", async route => {
    const path = new URL(route.request().url()).pathname;
    const body = route.request().postDataJSON();
    if (path.includes("/account/security")) return route.fallback();
    if (path.endsWith("/access/scopes")) return route.fulfill({ json: { token: "test-scope", expires: Date.now()/1000 + 300 } });
    if (path.endsWith("/trading-time")) return route.fulfill({ json: [time] });
    if (path.endsWith("/data/catalog")) {
      const type = new URL(route.request().url()).searchParams.get("type_id");
      if (type === "futures.contracts" && failOptions) { return route.fulfill({status:503,json:{detail:"资料选项暂不可用"}}); }
      const items = type === "futures.contracts" ? [{id:"fixed-contracts",manifest:{source:"offline",scope:{exchange:"SHFE"}}}] : type === "futures.daily" ? symbols.map((symbol, i) => ({ id:`daily-${i}`,manifest:{source:"offline",scope:{symbol,start:"2025-04-10",end:"2025-04-10"},contract_identity:{symbol}} })) : [];
      return route.fulfill({json:{items,total:items.length}});
    }
    if (path.endsWith("/candidates/preview")) {
      candidate = {request:body,coverage:"fixed_source_product",included:ids,excluded:[],catalog:{inputs:[{source:"offline"}],symbols:ids.map((contract_id,i)=>({contract_id,symbol:symbols[i]})),contracts:ids.map(id=>({id}))}};
      return route.fulfill({json:candidate});
    }
    if (path.endsWith("/computed/preview")) {
      requests.push(body);
      if (body.daily_inputs.some((r:any) => r.version_id === "missing")) return route.fulfill({status:422,json:{detail:"固定日线缺少唯一观测交易日"}});
      spec = {origin:"computed",request:body,candidates:candidate,artifact:{algorithm:"daily-role-ranking.v1",checksum:"fixture"},result:{input_digest:"fixture",decisions:[{observation_day:"2025-04-10",available_at:"2025-04-10T15:05:00+08:00",effective_day:"2025-04-11",effective_start:"2025-04-10T21:00:00+08:00",main:ids[1],secondary:ids[0],reason:"initial",confirmation_count:0,excluded:[]}]}};
      return route.fulfill({json:spec});
    }
    if (path.endsWith("/computed/start")) {
      publications.push(body);
      saved ??= {id:"published-first",published_at:"2025-04-10T16:00:00+08:00",spec:body};
      if (publications.length === 1) return route.abort("failed");
      return route.fulfill({json:saved});
    }
    if (path.endsWith("/contract-roles/computed")) return route.fulfill({json:saved?[saved]:[]});
    if (path.endsWith("/sync-plan")) return route.fulfill({json:{previous_version_id:"published-first",trading_day:"2025-04-11",provider:"offline",symbols}});
    if (path.endsWith("/sync-workflows") || path.endsWith("/data/providers")) return route.fulfill({json:[]});
    return route.fulfill({json:path.endsWith("/login")?{session:"session",user}:path.endsWith("/me")?user:path.endsWith("/health")?{status:"ready"}:[]});
  });
  await page.goto("/");
  await page.getByLabel("邮箱", {exact:true}).fill(user.email);
  await page.getByLabel("密码", {exact:true}).fill("role-test-password");
  await page.getByRole("button", {name:"登录",exact:true}).click();
  await page.getByRole("navigation",{name:"业务工作区"}).getByRole("button",{name:"数据",exact:true}).click();
  await page.getByRole("button",{name:"合约角色",exact:true}).click();
  const panel = page.getByRole("region",{name:"合约角色诊断"});
  await expect(panel).toContainText("暂无计算角色");
  await panel.getByRole("button",{name:"首次计算",exact:true}).click();
  const form = page.getByRole("region",{name:"首次计算主力角色",exact:true});
  await expect(form.getByRole("alert")).toContainText("资料选项暂不可用");
  failOptions = false;
  await form.getByRole("button",{name:"重试读取选项"}).click();
  await expect(form.getByRole("alert")).toHaveCount(0);
  await form.getByLabel("合约资料版本",{exact:true}).fill("fixed-contracts");
  await form.getByLabel("交易时间版本",{exact:true}).selectOption("fixed-time");
  await form.getByLabel("观测交易日").fill("2025-04-10");
  await form.getByLabel("候选范围",{exact:true}).selectOption("product_catalog");
  await form.getByRole("button",{name:"核对候选合约"}).click();
  await form.getByLabel("AU2506 日线版本",{exact:true}).fill("daily-0");
  await form.getByLabel("AU2508 日线版本",{exact:true}).fill("missing");
  await expect(form.getByLabel("排名指标")).toHaveValue("");
  await expect(form.getByLabel("初始主力")).toHaveValue("");
  await form.getByLabel("排名指标").selectOption("open_interest");
  await form.getByLabel("同值排序").selectOption("earlier_delivery");
  await form.getByLabel("无成交处理").selectOption("exclude");
  await form.getByLabel("换月超越比例").fill("0.1");
  await form.getByLabel("连续确认次数").fill("2");
  await form.getByLabel("允许回到更早交割月").selectOption("no");
  await form.getByLabel("初始主力").selectOption("none");
  await form.getByLabel("计算依据说明").fill("离线界面测试，非真实市场验收");
  await form.getByRole("button",{name:"预览计算结果"}).click();
  await expect(form.getByRole("alert")).toContainText("固定日线缺少唯一观测交易日");
  expect(publications).toHaveLength(0);
  await form.getByLabel("AU2508 日线版本",{exact:true}).fill("daily-1");
  await form.getByRole("button",{name:"预览计算结果"}).click();
  const result = form.getByRole("region",{name:"首次计算预览"});
  await expect(result).toContainText("2025-04-10T21:00:00+08:00");
  await expect(form.getByLabel("排名指标")).toBeDisabled();
  await result.getByRole("button",{name:"修改输入"}).click();
  await expect(result).toHaveCount(0);
  await form.getByLabel("连续确认次数").fill("3");
  await form.getByRole("button",{name:"预览计算结果"}).click();
  expect(requests.at(-1).policy.confirmations).toBe(3);
  expect(requests.at(-1).trading_time).toEqual(time);
  expect(requests.at(-1).initial_main).toBeNull();
  await expect(result).toBeVisible();
  await expect(result.getByRole("button", { name: "确认发布" })).toBeEnabled();
  await page.screenshot({path:"../../.state/role-first-preview.png",fullPage:true});
  await result.getByRole("button",{name:"确认发布"}).click();
  await expect(form.getByRole("alert")).toContainText("发布未确认");
  await expect(result.getByRole("button",{name:"修改输入"})).toBeDisabled();
  await result.getByRole("button",{name:"重试发布相同结果"}).click();
  await expect(form).toHaveCount(0);
  expect(publications).toHaveLength(2);
  expect(publications[0]).toEqual(publications[1]);
  await expect(panel.getByLabel("角色版本",{exact:true})).toHaveValue("published-first");
  await panel.getByRole("button",{name:"采集与续算",exact:true}).click();
  await expect(panel.getByRole("region",{name:"采集与续算"})).toContainText("2025-04-11");
  await page.reload();
  await page.getByRole("navigation",{name:"业务工作区"}).getByRole("button",{name:"数据",exact:true}).click();
  await page.getByRole("button",{name:"合约角色",exact:true}).click();
  await expect(panel.getByLabel("角色版本",{exact:true})).toHaveValue("published-first");
  expect(publications).toHaveLength(2);
});
