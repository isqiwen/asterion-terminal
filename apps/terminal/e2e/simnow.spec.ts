import {expect,test} from "@playwright/test";
import {nativeContext} from "./native";
test("named profiles persist and switch a single source across all panels",async({page,context})=>{
 await nativeContext(context);
 const fields=[{key:"broker_id",label:"BrokerID",identity:true},{key:"user_id",label:"投资者代码",identity:true},{key:"front",label:"行情前置"},{key:"trade_front",label:"交易前置"},{key:"app_id",label:"AppID"},{key:"auth_code",label:"认证码",secret:true},{key:"password",label:"账户密码",secret:true}].map(f=>({required:true,secret:false,identity:false,default:"",...f}));
 const features=["market_quotes","instrument_catalog","account_snapshot","positions"];
 const descriptors=[{id:"ctp",owner:"asterion.connector.ctp",version:1,title:"CTP",capabilities:features,fields}, {id:"second",owner:"test.second",version:1,title:"第二测试接入",capabilities:["market_quotes"],fields:[{key:"endpoint",label:"服务地址",required:true,secret:false,identity:false,default:""}]}];
 type Row={connection_id:string;connector_id:string;name:string;config_revision:number;config:Record<string,string>;secret_saved:Record<string,boolean>;capabilities:string[];available:boolean;market:{state:string;detail:string;generation:number};account:{state:string;detail:string;generation:number}};
 const rows:Row[]=[];const lists:Record<string,{exchange:string;symbol:string}[]>={};let missingNames=false,accountFailure=false,loadingUntil=0;
 let selectedId:string|null=null,activeId:string|null=null;
 const writes:any[]=[];
 await context.route("**/api/v1/**",route=>{
  const url=new URL(route.request().url()),path=url.pathname,body=route.request().postDataJSON();
  if(path.includes("/account/security"))return route.fallback();
  if(path.endsWith("/access/scopes"))return route.fulfill({json:{token:"fixture",expires:Date.now()/1000+300}});
  if(path==="/api/v1/connections"){
   if(route.request().method()==="POST"){
    writes.push(body);const id=body.connection_id??String(rows.length+1).padStart(32,"0");
    const prior=rows.find(r=>r.connection_id===id);
    const descriptor=descriptors.find(d=>d.id===body.connector_id)!;
    const row:Row={connection_id:id,connector_id:body.connector_id,name:body.name,config_revision:(prior?.config_revision??0)+1,config:body.config,secret_saved:Object.fromEntries(Object.entries(body.secrets).map(([k,v]:[string,any])=>[k,v.action!=="clear"])),capabilities:descriptor.capabilities,available:true,market:{state:"disconnected",detail:"已断开",generation:0},account:{state:"disconnected",detail:"已断开",generation:0}};
    if(prior)rows.splice(rows.indexOf(prior),1,row);else rows.push(row);
    selectedId??=id;lists[id]??=[];return route.fulfill({json:row});
   }
   return route.fulfill({json:{connections:rows,connectors:descriptors,selected_id:selectedId,active_id:activeId,notice:""}});
  }
  if(path.startsWith("/api/v1/connections/")){
   const [id,action]=path.slice("/api/v1/connections/".length).split("/");
   const row=rows.find(r=>r.connection_id===id)!;
   if(action==="delete"){
    if(activeId===id)return route.fulfill({status:409,json:{detail:"请先断开连接再删除配置"}});
    if(row.config_revision!==body.expected_revision)return route.fulfill({status:409,json:{detail:"配置已改变，请重新加载后删除"}});
    rows.splice(rows.indexOf(row),1);if(selectedId===id)selectedId=null;
    return route.fulfill({json:{connections:rows,connectors:descriptors,selected_id:selectedId,active_id:activeId,notice:""}});
   }
   const connect=action==="connect"||(action==="select"&&activeId!==null);
   for(const r of rows)for(const c of ["market","account"] as const)r[c]={state:"disconnected",detail:"已断开",generation:r[c].generation+1};
   activeId=null;
   if(action!=="disconnect")selectedId=id;
   if(connect){activeId=id;row.market={state:"ready",detail:"行情已登录",generation:row.market.generation};if(row.capabilities.includes("account_snapshot"))row.account={state:"ready",detail:"账户数据可用",generation:row.account.generation};}
   return route.fulfill({json:row});
  }
  const id=url.searchParams.get("connection_id")??"",row=rows.find(r=>r.connection_id===id);
  if(path.startsWith("/api/v1/market/")||path.startsWith("/api/v1/trading/")){
   if(!row)return route.fulfill({status:409,json:{detail:"连接不存在"}});
   if(path.includes("/contracts")){
    if(path.endsWith("/refresh"))loadingUntil=Date.now()+1500;
    const exchange=url.searchParams.get("exchange");
    const contracts=exchange==="SHFE"?[{exchange:"SHFE",symbol:"ad2610",name:"铸铝2610",product:"ad",delivery_month:"2026-10",listed_on:"2026-01-01",last_trade_on:"2026-10-15"},{exchange:"SHFE",symbol:"au2612",name:"黄金2612",product:"au",delivery_month:"2026-12",listed_on:"2026-01-01",last_trade_on:"2026-12-15"}]:[];
    return route.fulfill({json:{exchange,as_of:"2026-09-22",source:"连接合约目录",state:Date.now()<loadingUntil?"loading":"ready",detail:Date.now()<loadingUntil?"查询中":"已更新",observed_at:"2026-09-22T09:00:00+08:00",contracts}});
   }
   if(path.includes("/trading/"))return route.fulfill({json:{connection_id:id,connection_name:row.name,state:row.account.state==="disconnected"?"disconnected":accountFailure?"error":"ready",detail:row.account.detail,stale:accountFailure,observed_at:Date.now()/1000,account:row.account.state==="disconnected"?null:{currency:"CNY",trading_day:"20260922",balance:"100000",available:"80000",margin:"20000",position_profit:"10",close_profit:"0",commission:"1"},positions:[],margin_ratio:row.account.state==="disconnected"?null:"20",available_ratio:"80",risk_detail:"总体风险尚未完成评估"}});
   if(path.endsWith("/watchlist"))lists[id]=body.subscriptions;
   return route.fulfill({json:{connection_id:id,connection_name:row.name,state:row.market.state==="ready"?"connected":row.market.state,detail:row.market.detail,configuration:{subscriptions:lists[id]},quotes:row.market.state==="ready"?lists[id].map(s=>({...s,last:s.symbol==="ad2610"?101:99.5,previous_settlement:100,change:s.symbol==="ad2610"?1:-0.5,change_percent:s.symbol==="ad2610"?1:-0.5,high:102,low:99,volume:1,open_interest:2,trading_day:"20260922",action_day:"20260922",source_time:"09:30:00",status:"current",event_at:Date.now()/1000,received_at:Date.now()/1000,stale:false})):[],subscription_errors:{},contract_names:missingNames?{}:{"SHFE.ad2610":"铸铝2610","SHFE.au2612":"黄金2612"},observed_at:Date.now()/1000}});
  }
  const user={email:"connections@example.com",first_name:"Test",last_name:"User"};
  return route.fulfill({json:path.endsWith("/login")?{session:"session",user}:path.endsWith("/me")?user:[]});
 });
 await page.goto('/?view=总览');await page.getByLabel('邮箱',{exact:true}).fill('connections@example.com');await page.getByLabel('密码',{exact:true}).fill('fixture');await page.getByRole('button',{name:'登录',exact:true}).click();
 const portfolio=page.getByRole('region',{name:'账户与持仓',exact:true});
 const quoteRegion=page.getByRole('region',{name:'实时行情',exact:true});
 const settingsPromise=context.waitForEvent('page');await quoteRegion.getByRole('button',{name:'配置连接'}).click();const settings=await settingsPromise;
 await settings.getByRole('button',{name:'添加配置'}).click();
 const form=settings.getByRole('form',{name:'连接配置'});
 await form.getByLabel('连接名称').fill('SimNow测试');
 for(const [label,value] of [['BrokerID','9999'],['投资者代码','fixture'],['行情前置','tcp://fixture:1234'],['交易前置','tcp://fixture:1235'],['AppID','simnow_client_test'],['认证码','0000000000000000'],['账户密码','secret-fixture']])await form.getByLabel(label,{exact:true}).fill(value);
 await form.getByRole('button',{name:'保存配置',exact:true}).click();
 await settings.getByRole('region',{name:'账户连接'}).getByRole('button',{name:'连接',exact:true}).click();
 await expect(portfolio).toContainText('100,000');
 await expect(settings.getByRole('button',{name:'编辑配置'})).toBeDisabled();await expect(settings.getByRole('button',{name:'删除配置'})).toBeDisabled();
 await expect(quoteRegion.getByRole('button',{name:'断开连接',exact:true})).toBeVisible();
 await quoteRegion.getByRole('button',{name:'自选合约'}).click();const dialog=page.getByRole('dialog',{name:'自选合约'});
 await dialog.getByLabel('搜索合约').fill('ad2610');await expect(dialog.getByRole('checkbox',{name:'选择 铸铝2610'})).toBeVisible();
 await dialog.getByLabel('搜索合约').fill('铸铝');await dialog.getByRole('checkbox',{name:'选择 铸铝2610'}).check();
 await dialog.getByLabel('搜索合约').fill('');await dialog.getByRole('checkbox',{name:'选择 黄金2612'}).check();await dialog.getByRole('button',{name:'保存自选'}).click();
 const table=page.getByRole('table',{name:'实时报价'});await expect(table.getByRole('cell',{name:'铸铝2610',exact:true})).toBeVisible();await expect(table).not.toContainText('ad2610');
 missingNames=true;await expect(table.getByRole('cell',{name:'ad2610',exact:true})).toBeVisible();missingNames=false;
 await settings.getByRole('button',{name:'断开连接',exact:true}).click();await expect(portfolio).not.toContainText('100,000');
 await expect(quoteRegion.getByRole('button',{name:'连接',exact:true})).toBeVisible();
 await settings.getByRole('region',{name:'账户连接'}).getByRole('button',{name:'连接',exact:true}).click();accountFailure=true;await expect(portfolio).toContainText('数据已陈旧');accountFailure=false;
 await page.screenshot({path:'../../.state/connections-overview.png'});
 await settings.getByRole('button',{name:'添加配置'}).click();await form.getByLabel('接入协议').selectOption('second');await form.getByLabel('连接名称').fill('第二来源');await form.getByLabel('服务地址').fill('second');await form.getByRole('button',{name:'保存配置'}).click();
 await expect(settings.getByLabel('当前连接')).toContainText('第二来源');
 await settings.getByLabel('当前连接').selectOption(rows[1].connection_id);
 await expect(table).not.toContainText('铸铝2610');await expect(portfolio).not.toContainText('100,000');expect(rows[0].market.state).toBe('disconnected');expect(rows[0].account.state).toBe('disconnected');
 await page.getByLabel('当前连接').selectOption(rows[0].connection_id);await expect(table).toContainText('铸铝2610');
 await page.reload();await expect(table).toContainText('铸铝2610');
 await quoteRegion.getByRole('button',{name:'自选合约'}).click();await dialog.getByText('已选合约（2）',{exact:true}).click();await dialog.getByRole('button',{name:'移除铸铝2610'}).click();await dialog.getByRole('button',{name:'保存自选'}).click();await expect(table).not.toContainText('铸铝2610');
 await settings.getByRole('button',{name:'断开连接',exact:true}).click();
 await settings.getByRole('button',{name:'编辑配置'}).click();await expect(form.getByLabel('账户密码',{exact:true})).toHaveAttribute('readonly','');
 await settings.screenshot({path:'../../.state/connection-secret-edit.png'});
 await form.getByRole('button',{name:'修改认证码',exact:true}).click();await form.getByLabel('认证码',{exact:true}).fill('new-auth');await form.getByRole('button',{name:'取消修改认证码'}).click();
 await form.getByRole('button',{name:'修改账户密码',exact:true}).click();await form.getByLabel('账户密码',{exact:true}).fill('changed-secret');
 await form.getByLabel('连接名称').fill('我的账户');await form.getByRole('button',{name:'保存配置'}).click();
 await expect(page.getByLabel('当前连接')).toContainText('我的账户');
 expect(writes).toHaveLength(3);expect(writes[2].secrets.password).toEqual({action:'replace',value:'changed-secret'});expect(writes[2].secrets.auth_code).toEqual({action:'keep'});expect(writes[0].secrets.password).toEqual({action:'replace',value:'secret-fixture'});
 expect(await page.evaluate(()=>JSON.stringify(localStorage))).not.toContain('secret-fixture');
 await settings.getByRole('button',{name:'删除配置'}).click();const deletion=settings.getByRole('dialog',{name:'删除连接配置'});
 await expect(deletion).toContainText('我的账户');await settings.screenshot({path:'../../.state/connections-delete-confirm.png'});await deletion.getByRole('button',{name:'取消',exact:true}).click();expect(rows).toHaveLength(2);
 await settings.getByRole('button',{name:'删除配置'}).click();await deletion.getByRole('button',{name:'确认删除'}).click();
 await expect(settings.getByLabel('当前连接')).not.toContainText('我的账户');await expect(page.getByLabel('当前连接')).toHaveValue('');await expect(table).not.toContainText('黄金2612');
 await page.reload();await expect(page.getByLabel('当前连接')).not.toContainText('我的账户');expect(rows).toHaveLength(1);
 await settings.screenshot({path:'../../.state/connections-delete.png'});
});
