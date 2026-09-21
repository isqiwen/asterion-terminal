import {expect,test} from "@playwright/test";
import {nativeContext} from "./native";
test("SimNow configuration connects, quotes expire, errors stay visible and password is not saved",async({page,context})=>{
 await nativeContext(context);
 let configuration={version:1,front:"",user_id:"",subscriptions:[] as {exchange:string;symbol:string}[]};
 let connected=false,expired=false,failed=false;
 const saves:unknown[]=[];
 await context.route("**/api/v1/**",route=>{
  const path=new URL(route.request().url()).pathname;
  if(path.includes("/account/security"))return route.fallback();
  if(path.endsWith("/access/scopes"))return route.fulfill({json:{token:"fixture",expires:Date.now()/1000+300}});
  if(path.includes("/market/")){
   if(failed&&path.endsWith("/state"))return route.fulfill({status:503,json:{detail:"行情服务不可用"}});
   if(path.endsWith("/configuration")){configuration=route.request().postDataJSON();saves.push(configuration);}
   if(path.endsWith("/connect"))connected=true;
   if(path.endsWith("/disconnect"))connected=false;
   return route.fulfill({json:{environment:"simnow",state:connected?"connected":"disconnected",detail:connected?"SimNow 已登录":"行情源未连接",configuration,quotes:connected?[{exchange:"SHFE",symbol:"au2612",last:800,change_percent:1.25,volume:10,open_interest:20,trading_day:"20260921",source_time:"09:30:00",event_at:Date.now()/1000,received_at:Date.now()/1000,stale:expired}]:[],subscription_errors:{},observed_at:Date.now()/1000}});
  }
  const user={email:"simnow@example.com",first_name:"Test",last_name:"User"};
  return route.fulfill({json:path.endsWith('/login')?{session:"session",user}:path.endsWith('/me')?user:[]});
 });
 await page.goto('/?view=总览');
 await page.getByLabel('邮箱',{exact:true}).fill('simnow@example.com');
 await page.getByLabel('密码',{exact:true}).fill('local-fixture-password');
 await page.getByRole('button',{name:'登录',exact:true}).click();
 await page.getByRole('button',{name:'连接与自选',exact:true}).click();
 const dialog=page.getByRole('dialog',{name:'SimNow 行情连接'});
 await dialog.getByLabel('行情前置',{exact:true}).fill('tcp://localhost:12345');
 await dialog.getByLabel('SimNow 投资者代码').fill('123456');
 await dialog.getByLabel('SimNow 密码').fill('never-persist-me');
 await dialog.getByLabel('自选合约代码').fill('au2612');
 await dialog.getByRole('button',{name:'添加自选',exact:true}).click();
 await page.screenshot({path:'../../.state/simnow-connection.png'});
 await dialog.getByRole('button',{name:'保存并连接'}).click();
 await expect(dialog).toHaveCount(0);
 const table=page.getByRole('table',{name:'实时报价'});
 await expect(table).toContainText('SHFE · au2612');
 await expect(table).toContainText('更新中');
 expect(JSON.stringify(saves)).not.toContain('never-persist-me');
 expect(await page.evaluate(()=>JSON.stringify(localStorage))).not.toContain('never-persist-me');
 expired=true;await expect(table).toContainText('已过期');
 failed=true;await expect(page.getByRole('alert').filter({hasText:'行情状态读取失败'})).toBeVisible();
 failed=false;
 await page.getByRole('button',{name:'连接与自选',exact:true}).click();
 await expect(dialog.getByLabel('SimNow 密码')).toHaveValue('');
 await dialog.getByRole('button',{name:'断开行情',exact:true}).click();
 await expect(dialog.getByLabel('行情前置',{exact:true})).toBeEnabled();
 await dialog.getByRole('button',{name:'移除au2612'}).click();
 await dialog.getByRole('button',{name:'保存配置',exact:true}).click();
 await expect(table).not.toContainText('au2612');
});
