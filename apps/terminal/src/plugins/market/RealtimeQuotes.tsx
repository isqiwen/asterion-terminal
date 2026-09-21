import { useEffect, useRef, useState } from "react";
import type { RequestClient } from "../../api/requests";
import { DashboardTable } from "../overview/DashboardTable";
import type { MarketConfiguration, MarketState } from "./live";
export const quoteColumns = ["合约", "最新价", "涨跌", "成交量", "持仓量", "行情时间", "状态"].map((title, i) => ({ title, numeric: i > 0 && i < 6 }));
const fmt = (n: number | null | undefined) => n == null ? "—" : n.toLocaleString("zh-CN",{maximumFractionDigits:4});
function ConnectionDialog({api, state, close, changed}: {api:RequestClient; state:MarketState; close:()=>void; changed:(s:MarketState)=>void}) {
  const dialog=useRef<HTMLDialogElement>(null);
  const [draft,setDraft]=useState<MarketConfiguration>(state.configuration);
  const [password,setPassword]=useState("");
  const [busy,setBusy]=useState(false);
  const [error,setError]=useState("");
  const [exchange,setExchange]=useState<"SHFE"|"DCE"|"CZCE"|"CFFEX"|"INE"|"GFEX">("SHFE");
  const [symbol,setSymbol]=useState("");
  const active=["connected","connecting","reconnecting","error"].includes(state.state);
  useEffect(()=>{ const previous=document.activeElement;dialog.current?.showModal();return()=>{dialog.current?.close();if(previous instanceof HTMLElement)previous.focus();};},[]);
  async function run(connect:boolean) {
    setBusy(true);setError("");
    try {
      const saved=await api.request<MarketState>("/market/configuration",draft);changed(saved);
      if(connect) { const secret=password;setPassword("");changed(await api.request<MarketState>("/market/connect",{password:secret})); }
      close();
    } catch(e) {setError(String(e));} finally {setBusy(false);}
  }
  return <dialog ref={dialog} className="widget-catalog" aria-label="SimNow 行情连接" onCancel={e=>{e.preventDefault();if(!busy)close();}}>
    <header><h2>SimNow 行情</h2><button disabled={busy} onClick={close}>关闭</button></header>
    <p className="dashboard-caption">仿真环境 · 仅行情。密码仅保留在本次连接内存中，重启后重新输入。</p>
    <p className="dashboard-caption">从 SimNow 官网复制交易时段环境的 Market Front。BrokerID 为 9999；不要填写交易前置或 7×24 测试环境。</p>
    {error&&<p role="alert" className="notice">{error}</p>}
    {active&&<div className="dashboard-controls"><span>修改配置或自选前请先断开。</span><button disabled={busy} onClick={async()=>{setBusy(true);try{changed(await api.request<MarketState>("/market/disconnect",{}));}catch(e){setError(String(e));}finally{setBusy(false);}}}>断开行情</button></div>}
    <fieldset disabled={busy||active} className="live-configuration">
      <label>行情前置<input aria-label="行情前置" placeholder="tcp://主机:端口" value={draft.front} onChange={e=>setDraft({...draft,front:e.target.value})}/></label>
      <label>SimNow 投资者代码<input aria-label="SimNow 投资者代码" autoComplete="off" value={draft.user_id} onChange={e=>setDraft({...draft,user_id:e.target.value})}/></label>
      <label>SimNow 密码<input aria-label="SimNow 密码" type="password" autoComplete="off" value={password} onChange={e=>setPassword(e.target.value)}/></label>
      <strong>自选合约</strong>
      <div className="dashboard-controls"><select aria-label="自选交易所" value={exchange} onChange={e=>setExchange(e.target.value as typeof exchange)}>{["SHFE","DCE","CZCE","CFFEX","INE","GFEX"].map(e=><option key={e}>{e}</option>)}</select><input aria-label="自选合约代码" placeholder="实际月份合约，大小写按来源" value={symbol} onChange={e=>setSymbol(e.target.value)}/><button disabled={!symbol.trim()||draft.subscriptions.length>=50} onClick={()=>{const code=symbol.trim();if(draft.subscriptions.some(s=>s.symbol===code)){setError("此合约已在自选中");return;}setDraft({...draft,subscriptions:[...draft.subscriptions,{exchange,symbol:code}]});setSymbol("");}}>添加自选</button></div>
      <ul className="dashboard-list">{draft.subscriptions.map(s=><li key={s.symbol}><div><span>{s.exchange} · {s.symbol}</span><button aria-label={`移除${s.symbol}`} onClick={()=>setDraft({...draft,subscriptions:draft.subscriptions.filter(i=>i.symbol!==s.symbol)})}>移除</button></div></li>)}</ul>
      <div className="dashboard-controls"><button onClick={()=>void run(false)}>保存配置</button><button disabled={!draft.front||!draft.user_id||!password} onClick={()=>void run(true)}>保存并连接</button></div>
    </fieldset>
  </dialog>;
}
export function RealtimeQuotes({api}:{api:RequestClient}) {
  const [state,setState]=useState<MarketState|null>(null);
  const [error,setError]=useState("");
  const [editing,setEditing]=useState(false);
  useEffect(()=>{let disposed=false;let timer:ReturnType<typeof setTimeout>;setState(null);setError("");
    const poll=async()=>{try{const next=await api.request<MarketState>("/market/state");if(!next || next.environment!=="simnow" || !next.configuration || !Array.isArray(next.configuration.subscriptions) || !Array.isArray(next.quotes) || !next.subscription_errors) throw new Error("行情状态不符合当前接口契约");if(!disposed){setState(next);setError("");}}catch(e){if(!disposed)setError(`行情状态读取失败：${String(e)}`);}finally{if(!disposed)timer=setTimeout(poll,1000);}};
    void poll();return()=>{disposed=true;clearTimeout(timer);};
  },[api]);
  const rows=(state?.configuration.subscriptions??[]).map(s=>{
    const q=state?.quotes.find(q=>q.symbol===s.symbol);
    const failed=state?.subscription_errors[s.symbol];
    const stale=error||state?.state!=="connected"||q?.stale;
    return {id:s.symbol,cells:[`${s.exchange} · ${s.symbol}`,fmt(q?.last),q?.change_percent==null?"—":`${q.change_percent>0?"+":""}${q.change_percent.toFixed(2)}%`,fmt(q?.volume),fmt(q?.open_interest),q?<span title={`交易日 ${q.trading_day}；涨跌基于昨结算价`}>{q.source_time}</span>:"—",failed||(!q?"等待报价":stale?"已过期 / 时间未确认":"更新中")]};
  });
  return <>
    <div className="dashboard-controls"><span role="status">{state?`SimNow · ${state.detail}`:"正在读取行情状态…"}</span><span className="panel-spacer"/><button disabled={!state} onClick={()=>setEditing(true)}>连接与自选</button></div>
    {error&&<p role="alert" className="dashboard-caption">{error}</p>}
    <DashboardTable label="实时报价" columns={quoteColumns} rows={rows} empty={state?.state==="connected"?"已登录 SimNow；请在“连接与自选”添加月份合约。":"尚无报价；请配置 SimNow 连接与自选合约。"}/>
    {editing&&state&&<ConnectionDialog api={api} state={state} changed={setState} close={()=>setEditing(false)}/>}
  </>;
}
