import { coverageIdentity } from "./reference-fixture";
import { providerFixture } from "./provider-fixture";
import { test, expect } from "@playwright/test";
import { nativeContext } from "./native";

test("same-connection research preparation, lost response, restart, isolated retry and fixed coverage", async ({context,page}) => {
  await nativeContext(context);
  const connection = "c_" + "a".repeat(32);
  const provider = {id:connection,plugin_id:"tushare",connection_id:connection,name:"研究账户",configured:true,version:"1",api_version:2,capabilities:[
    {id:"daily",type_id:"futures.daily",label:"历史日线",exchanges:["SHFE","GFEX"],date_range:true,symbol_required:true},
    {id:"calendar",type_id:"futures.calendar",label:"日历",exchanges:["SHFE"],date_range:true,symbol_required:false},
    {id:"contracts",type_id:"futures.contracts",label:"合约",exchanges:["SHFE","GFEX"],date_range:false,symbol_required:false},
  ]};
  let batch: any = null, first = true, checks = 0, retries = 0;
  const commands: string[] = [];
  const type = {id:"futures.daily",label:"历史日线",domain:"market",domain_label:"行情",shape:"timeseries",frequency:"1d",schema_version:1,primary_key:["symbol","trading_day"],time_semantics:"交易日",fields:[]};
  const version = {id:"daily-fixed",dataset_id:"daily-set",job_id:"daily-job",created_at:1,rows:3,manifest:{type,source:"tushare",layer:"STANDARD",scope:{symbol:"RB2610.SHF",exchange:"SHFE"},format:"parquet",checksum:"fixture",bytes:100,inputs:[],first:"2024-01-02",last:"2024-01-04",quality:"VALIDATED",coverage:"RETURNED_ROWS_ONLY",version_semantics:"CUMULATIVE",transform:null}};
  const report = {id:"batch-report",daily_version_id:version.id,calendar_version_id:"calendar-fixed",contracts_version_id:"contracts-fixed",start:"2024-01-02",end:"2024-01-04",status:"COVERED",counts:{PRESENT:3},days:[],notes:[],refill_ranges:[],source:"tushare",checker:"daily-coverage-v2",identity:coverageIdentity(),reference_symbol:null,created_at:1,as_of:"2026-09-20"};
  await context.route("**/api/v1/**", route => {
    if (route.request().url().endsWith("/access/scopes")) return route.fulfill({ json: { token: "scope-fixture", expires: Date.now() / 1000 + 300 } });

    const req = route.request(), path = new URL(req.url()).pathname;
    if (path.includes("/security")) return route.fallback();
    if (path.endsWith("/data/providers")) return route.fulfill({json:[providerFixture(provider)]});
    if (path.endsWith("/data/preparations")) {
      if (req.method() === "GET") return route.fulfill({json:batch ? [batch] : []});
      const body = req.postDataJSON(); commands.push(body.command_id);
      expect(body).toMatchObject({provider:"tushare",connection_id:connection,dataset:"daily",exchange:"SHFE",symbol:"RB2610.SHF",start:"2024-01-02",end:"2024-01-04"});
      expect(body).not.toHaveProperty("token");
      batch ??= {daily_state:"SUBMITTED",identity:coverageIdentity(),identity_error:null,id:body.command_id,request:body,connection_name:provider.name,created_at:1,truncated:false,tasks:["daily","calendar","contracts"].map(kind => ({type_id:`futures.${kind}`,job:{id:`${kind}-job`,command_id:kind,kind:"data.sync",state:kind === "calendar" ? "FAILED":"SUCCEEDED",attempt:1,created_at:1,error:kind === "calendar" ? "日历权限不足":null,result:kind === "calendar" ? null:{version_id:`${kind}-fixed`}}}))};
      if (first) {first = false; return route.fulfill({status:503,json:{detail:"响应丢失，请重试"}});}
      return route.fulfill({json:batch,status:202});
    }
    if (path.endsWith("/data/jobs/calendar-job/retry")) {
      retries++; expect(req.postDataJSON().resume).toBe(true);
      const task = batch.tasks.find((t:any) => t.type_id === "futures.calendar");
      task.job = {...task.job,id:"calendar-retry",state:"SUCCEEDED",error:null,result:{version_id:"calendar-fixed"}};
      return route.fulfill({json:task.job});
    }
    if (path.endsWith("/coverage") && req.method() === "POST") {
      checks++;
      expect(path).toContain("daily-fixed");
      expect(req.postDataJSON()).toEqual({start:"2024-01-02",end:"2024-01-04",use_latest_daily:false,calendar_version_id:"calendar-fixed",contracts_version_id:"contracts-fixed"});
      return route.fulfill({json:report});
    }
    if (path.endsWith("/data/coverage/batch-report")) return route.fulfill({json:report});
    if (path.endsWith("/data/types")) return route.fulfill({json:[type]});
    if (path.includes("/data/catalog")) return route.fulfill({json:{items:[version],total:1,offset:0}});
    if (path.includes("/data/versions/")) return route.fulfill({json:{version,rows:[],total:3,offset:0,snapshot:null}});
    const user = {email:"prepare@example.com",first_name:"Prepare",last_name:"Test"};
    return route.fulfill({json:path.endsWith("/login") ? {session:"fixture-session",user} : path.endsWith("/me") ? user : path.endsWith("/health") ? {status:"ready"}:[]});
  });
  await page.goto("/");
  await page.getByLabel("邮箱",{exact:true}).fill("prepare@example.com");
  await page.getByLabel("密码",{exact:true}).fill("fixture-password");
  await page.getByRole("button",{name:"登录",exact:true}).click();
  await page.getByRole("button",{name:"数据",exact:true}).click();
  await page.getByRole("button",{name:"数据同步",exact:true}).click();
  await page.getByLabel("数据类型",{exact:true}).selectOption("daily");
  const together = page.getByRole("checkbox",{name:"同时准备研究依据（日历与合约资料）"});
  await page.getByLabel("交易所",{exact:true}).selectOption("GFEX");
  await expect(together).toBeDisabled();
  await page.getByLabel("交易所",{exact:true}).selectOption("SHFE");
  await together.check();
  await page.getByLabel("来源代码",{exact:true}).fill("RB2610.SHF");
  await page.getByLabel("开始日期",{exact:true}).fill("2024-01-02");
  await page.getByLabel("结束日期",{exact:true}).fill("2024-01-04");
  await page.getByRole("button",{name:"准备研究数据",exact:true}).click();
  await expect(page.getByRole("alert")).toContainText("响应丢失");
  await page.getByRole("button",{name:"准备研究数据",exact:true}).click();
  expect(commands[0]).toBe(commands[1]);
  const panel = page.getByRole("region",{name:"研究数据准备记录"});
  await expect(panel.getByText("日历权限不足",{exact:true})).toBeVisible();
  await expect(panel.getByRole("button",{name:"核对覆盖并查看数据",exact:true})).toBeDisabled();
  await page.reload();
  await page.getByRole("button",{name:"数据",exact:true}).click();
  await page.getByRole("button",{name:"数据同步",exact:true}).click();
  await expect(panel.getByText("日历权限不足",{exact:true})).toBeVisible();
  expect(commands.length).toBe(2);
  await panel.getByRole("button",{name:"继续同步交易日历",exact:true}).click();
  await expect(panel.getByText("已发布",{exact:true})).toHaveCount(3);
  expect(retries).toBe(1);
  await page.screenshot({path:"../../.state/research-preparation.png",fullPage:true});
  await panel.getByRole("button",{name:"核对覆盖并查看数据",exact:true}).click();
  await expect.poll(() => checks).toBe(1);
  await expect(page.getByRole("heading",{name:"历史日线 · 标准化预览"})).toBeVisible();
});

test("pending identity and rejected identity never appear ready for research", async ({context, page}) => {
  await nativeContext(context);
  let state = "WAITING_REFERENCE";
  await context.route("**/api/v1/**", route => {
    const path = new URL(route.request().url()).pathname;
    if (path.includes("/security")) return route.fallback();
    if (path.endsWith("/access/scopes")) return route.fulfill({json:{token:"scope",expires:Date.now()/1000+300}});
    if (path.endsWith("/data/preparations")) return route.fulfill({json:[{id:"waiting",request:{exchange:"SHFE",symbol:"RB2610.SHF",start:"2024-01-02",end:"2024-01-04"},connection_name:"fixture",created_at:1,truncated:false,daily_state:state,identity:null,identity_error:state === "IDENTITY_REJECTED" ? "固定合约资料无法确认完整且唯一的身份" : null,tasks:[]} ]});
    const user={email:"prepare@example.com",first_name:"Prepare",last_name:"Test"};
    return route.fulfill({json:path.endsWith("/login") ? {session:"fixture",user} : path.endsWith("/me") ? user : path.endsWith("/health") ? {status:"ready"} : []});
  });
  await page.goto("/");
  await page.getByLabel("邮箱",{exact:true}).fill("prepare@example.com");
  await page.getByLabel("密码",{exact:true}).fill("fixture-password");
  await page.getByRole("button",{name:"登录",exact:true}).click();
  await page.getByRole("button",{name:"数据",exact:true}).click();
  await page.getByRole("button",{name:"数据同步",exact:true}).click();
  const panel=page.getByRole("region",{name:"研究数据准备记录"});
  await expect(panel.getByText("日线等待合约资料通过身份校验")).toBeVisible();
  await expect(panel.getByRole("button",{name:"核对覆盖并查看数据"})).toBeDisabled();
  state="IDENTITY_REJECTED";
  await expect(panel.getByRole("alert")).toContainText("固定合约资料无法确认完整且唯一的身份");
  await expect(panel.getByRole("button",{name:"核对覆盖并查看数据"})).toBeDisabled();
});
