import {useConnections} from "../connections/public";
import { useEffect, useState } from "react";
import type { RequestClient } from "../../api/requests";
import { DashboardTable } from "../overview/DashboardTable";
import type { MarketState } from "./live";
export const quoteColumns = ["合约", "最新价", "涨跌额", "涨跌幅", "最高", "最低", "成交量", "持仓量", "行情时间", "状态"].map((title, i) => ({ title, numeric: i > 0 && i < 9 }));
const fmt = (n: number | null | undefined) => n == null ? "—" : n.toLocaleString("zh-CN",{maximumFractionDigits:4});
import { WatchlistDialog } from "./WatchlistDialog";
import { useMarketState } from "./useMarketState";
import { openSettings } from "../../settings/window";
export function RealtimeQuotes({api}:{api:RequestClient}) {
  const connections=useConnections(api);
  const selected=connections.selected;
  const connectionId=selected?.connection_id??"";
  const {state:settledState,setState,error}=useMarketState(api,connectionId);
  const state=settledState?.connection_id===connectionId?settledState:null;
  const [editing,setEditing]=useState(false);
  const [busy,setBusy]=useState(false);
  const [actionError,setActionError]=useState("");
  useEffect(()=>setActionError(""),[state?.state,connectionId]);
  const active=!!connectionId&&connections.data?.active_id===connectionId;
  async function toggle(){
    setBusy(true);setActionError("");
    try{await api.request(`/connections/${connectionId}/${active?"disconnect":"connect"}`,{});}
    catch(e){setActionError(String(e));}finally{setBusy(false);}
  }
  const rows=(state?.configuration.subscriptions??[]).map(s=>{
    const q=state?.quotes.find(q=>q.symbol===s.symbol);
    const failed=state?.subscription_errors[s.symbol];
    const name=state?.contract_names[`${s.exchange}.${s.symbol}`];
    const age=q?Math.max(0,Math.floor((state!.observed_at-q.received_at))):0;
    const status=!q?"等待报价":state?.state!=="connected"?"已断开":q.status==="time_unknown"?"来源日期待核验":q.status==="time_ahead"?"来源时间超前":q.status==="not_updated"?`已 ${age} 秒未更新`:q.status==="delayed"?"来源报价延迟":error?"行情状态读取失败":"更新中";
    const change=q?.change;
    const percent=q?.change_percent;
    const color=change==null||change===0?"var(--muted)":change>0?"var(--quote-up)":"var(--quote-down)";
    return {id:s.symbol,cells:[<strong>{name??s.symbol}</strong>,<span style={{color:q?.last==null?"var(--muted)":color}}>{fmt(q?.last)}</span>,<span style={{color}}>{change==null?"—":`${change>0?"+":""}${fmt(change)}`}</span>,<span style={{color}}>{percent==null?"—":`${percent>0?"+":""}${percent.toFixed(2)}%`}</span>,fmt(q?.high),fmt(q?.low),fmt(q?.volume),fmt(q?.open_interest),q?<span title={`来源日期 ${q.action_day}；交易日 ${q.trading_day}；本机接收 ${new Date(q.received_at*1000).toLocaleString("zh-CN")}；涨跌基于昨结算价`}>{q.source_time}</span>:"—",failed||status]};
  });
  return <>
    <div className="dashboard-controls"><select aria-label="当前连接" disabled={busy} value={connectionId} onChange={e=>{setBusy(true);setActionError("");setEditing(false);void connections.select(e.target.value).catch(e=>setActionError(String(e))).finally(()=>setBusy(false));}}><option value="" disabled>选择配置</option>{connections.data?.connections.map(c=><option key={c.connection_id} value={c.connection_id}>{c.name}</option>)}</select><span role="status">{state?`${state.detail}`:"请选择连接"}</span><span className="panel-spacer"/><button disabled={!state||busy||(!active&&!selected?.available)} onClick={()=>void toggle()}>{busy?"切换中…":active?"断开连接":"连接"}</button><button onClick={()=>void openSettings("连接").catch(e=>setActionError(String(e)))}>配置连接</button><button disabled={!state} onClick={()=>setEditing(true)}>自选合约</button></div>
    {(error||connections.error||actionError)&&<p role="alert" className="dashboard-caption">{error||connections.error||actionError}</p>}
    <DashboardTable label="实时报价" columns={quoteColumns} rows={rows} empty={state?.state==="connected"?"行情已登录；请在“自选合约”添加月份合约。":"尚无报价；请选择连接与自选合约。"}/>
    {editing&&state&&<WatchlistDialog openConnection={()=>void openSettings("连接").catch(e=>setActionError(String(e)))} api={api} state={state} changed={setState} close={()=>setEditing(false)}/>}
  </>;
}
