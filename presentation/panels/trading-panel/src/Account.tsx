import {useConnections} from "@asterion/ui-connection-settings/public";
import {useEffect,useState} from "react";
import type {RequestClient,RequestGrant} from "@asterion/runtime-client/requests";
import type {components} from "@asterion/api-types/schema";
import {DashboardTable} from "@asterion/ui-overview/DashboardTable";
import {openSettings} from "@asterion/workbench/settings/window";
export const tradingRequests:readonly RequestGrant[]=[{path:"/trading",descendants:true,methods:["GET","POST"]},{path:"/connections",descendants:false,methods:["GET"]}];
type State=components["schemas"]["AccountState"];
const money=(v:string|number|null|undefined)=>v==null?"—":Number(v).toLocaleString("zh-CN",{maximumFractionDigits:2});
const profitColor=(v:string|number|null|undefined)=>v==null||Number(v)===0?"var(--muted)":Number(v)>0?"var(--quote-up)":"var(--quote-down)";
export const positionColumns=["合约","方向 / 类别","手数","今仓","昨仓","保证金","持仓盈亏"].map((title,i)=>({title,numeric:i>1}));
function useAccount(api:RequestClient,connectionId:string){
 const [state,setState]=useState<State|null>(null),[error,setError]=useState("");
 useEffect(()=>{setState(null);setError("");let disposed=false;let timer:ReturnType<typeof setTimeout>;
  const poll=async()=>{if(!connectionId)return;try{const value=await api.request<State>(`/trading/account?connection_id=${connectionId}`);if(!value||value.connection_id!==connectionId||!Array.isArray(value.positions))throw new Error("账户响应不符合契约");if(!disposed){setState(value);setError("");}}catch(e){if(!disposed)setError(String(e));}finally{if(!disposed)timer=setTimeout(poll,2000);}};
  void poll();return()=>{disposed=true;clearTimeout(timer);};},[api,connectionId]);
 return {state,setState,error,setError};
}
export function AccountPanel({api,risk=false}:{api:RequestClient;risk?:boolean}){
 const connections=useConnections(api); const connectionId=connections.selected?.connection_id??"";
 const {state:received,setState,error,setError}=useAccount(api,connectionId); const state=received?.connection_id===connectionId?received:null;const [busy,setBusy]=useState(false);
 const account=state?.account;
 const metrics=risk?[["保证金 / 权益",state?.margin_ratio==null?"—":`${money(state.margin_ratio)}%`],["可用资金 / 权益",state?.available_ratio==null?"—":`${money(state.available_ratio)}%`],["风险评估","未完成"]]:[["账户权益",money(account?.balance)],["可用资金",money(account?.available)],["保证金占用",money(account?.margin)],["持仓盈亏",money(account?.position_profit)]];
 const rows=(state?.positions??[]).map(p=>({id:`${p.exchange}.${p.symbol}.${p.direction}.${p.hedge}`,cells:[<strong>{p.name??p.symbol}</strong>,`${p.direction==="long"?"多":"空"} / ${{"1":"投机","2":"套利","3":"套保"}[p.hedge]??p.hedge}`,p.quantity,p.today,p.yesterday,money(p.margin),<span style={{color:profitColor(p.profit)}}>{money(p.profit)}</span>]}));
 async function refresh(){setBusy(true);setError("");try{setState(await api.request<State>(`/trading/account/refresh?connection_id=${connectionId}`,{}));}catch(e){setError(String(e));}finally{setBusy(false);}}
 return <>
  <p className="dashboard-caption" role="status">{connections.selected?connections.selected.name:"请选择账户连接"} · {state?.observed_at?`更新于 ${new Date(state.observed_at*1000).toLocaleTimeString("zh-CN")}`:"尚未获取账户"}{account?` · 交易日 ${account.trading_day} · CNY`:""}{(error||state?.stale)&&account?" · 数据已陈旧，请刷新":""}</p>
  <dl className="account-summary" aria-label={risk?"风险指标":"资金摘要"}>{metrics.map(([label,value])=><div key={label}><dt>{label}</dt><dd style={label==="持仓盈亏"?{color:profitColor(account?.position_profit)}:undefined}>{value}</dd></div>)}</dl>
  {risk?<p className="dashboard-caption">{state?.risk_detail??"等待账户数据；风险暂无法评估"}</p>:<>
   <DashboardTable label="持仓明细" columns={positionColumns} rows={rows} empty={state?.state==="ready"&&!state.stale?"当前无持仓":"持仓尚未确认"}/>
   {account&&<p className="dashboard-caption">平仓盈亏 <span style={{color:profitColor(account.close_profit)}}>{money(account.close_profit)}</span> · 手续费 {money(account.commission)}；持仓盈亏按柜台口径展示。</p>}
  </>}
  {error&&<p role="alert">账户读取失败：{error}</p>}
  <div className="account-connection"><span>{state?.detail??"正在读取账户状态…"}</span>{!risk&&<><button disabled={!connectionId||busy||state?.state==="loading"||state?.state==="disconnected"} onClick={()=>void refresh()}>{busy?"查询中…":"刷新账户"}</button><button onClick={()=>void openSettings("连接").catch(e=>setError(String(e)))}>配置连接</button></>}</div>
 </>;
}
