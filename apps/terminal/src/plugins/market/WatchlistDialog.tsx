import { useEffect, useRef, useState } from "react";
import type { RequestClient } from "../../api/requests";
import type { components } from "../../api/schema";
import type { MarketState } from "./live";
type Choices = components["schemas"]["ContractChoices"];
type Choice = components["schemas"]["SourceInstrument"];
const key = (s: {exchange:string;symbol:string}) => `${s.exchange}.${s.symbol}`;
export function WatchlistDialog({api,state,close,changed,openConnection}:{api:RequestClient;state:MarketState;close:()=>void;changed:(s:MarketState)=>void;openConnection:()=>void}) {
  const dialog=useRef<HTMLDialogElement>(null);
  const [draft,setDraft]=useState(state.configuration.subscriptions);
  const [busy,setBusy]=useState(false);
  const [error,setError]=useState("");
  const [exchange,setExchange]=useState("SHFE");
  const [search,setSearch]=useState("");
  const [product,setProduct]=useState("");
  const [revision,setRevision]=useState(0);
  const [catalog,setCatalog]=useState<Choices|null>(null);
  const [loading,setLoading]=useState(true);
  const [loadError,setLoadError]=useState("");
  const [names,setNames]=useState<Record<string,string>>(state.contract_names);
  useEffect(()=>{const previous=document.activeElement;dialog.current?.showModal();return()=>{dialog.current?.close();if(previous instanceof HTMLElement)previous.focus();};},[]);
  useEffect(()=>{
    let disposed=false;let timer:ReturnType<typeof setTimeout>;setLoading(true);setCatalog(null);setLoadError("");setProduct("");
    const poll=async()=>{
      try {
        const value=await api.request<Choices>(`/market/contracts?connection_id=${state.connection_id}&exchange=${exchange}`);
        if(disposed)return;
        if(value.exchange!==exchange||!Array.isArray(value.contracts))throw new Error("合约列表不符合当前接口契约");
        setCatalog(value);setLoadError("");setNames(previous=>({...previous,...Object.fromEntries(value.contracts!.map(c=>[key(c),c.name]))}));
      } catch(e){if(!disposed)setLoadError(`合约列表读取失败：${String(e)}`);}
      finally{if(!disposed){setLoading(false);timer=setTimeout(poll,1000);}}
    };
    void poll();return()=>{disposed=true;clearTimeout(timer);};
  },[api,exchange,revision]);
  const choices = catalog?.exchange===exchange ? catalog.contracts??[] : [];
  async function refresh(){
    setError("");setLoading(true);
    try{await api.request(`/market/contracts/refresh?connection_id=${state.connection_id}&exchange=${exchange}`,{});setRevision(r=>r+1);}
    catch(e){setError(String(e));setLoading(false);}
  }
  const selected=new Set(draft.map(key));
  const filtered=choices.filter(c=>(!product||c.product===product)&&`${c.symbol} ${c.name} ${c.product}`.toLowerCase().includes(search.trim().toLowerCase()));
  function toggle(c:Choice){
    if(selected.has(key(c)))setDraft(draft.filter(s=>key(s)!==key(c)));
    else if(draft.length<50)setDraft([...draft,{exchange:c.exchange,symbol:c.symbol}]);
  }
  async function save(){
    setBusy(true);setError("");
    try{changed(await api.request<MarketState>(`/market/watchlist?connection_id=${state.connection_id}`,{subscriptions:draft}));close();}
    catch(e){setError(String(e));}finally{setBusy(false);}
  }
  return <dialog ref={dialog} className="widget-catalog" aria-label="自选合约" onCancel={e=>{e.preventDefault();if(!busy)close();}}>
    <header><h2>自选合约</h2><button disabled={busy} onClick={close}>关闭</button></header>
    {error&&<p role="alert" className="notice">{error}</p>}
    <fieldset disabled={busy} className="live-configuration">
      <div className="dashboard-controls">
        <select aria-label="自选交易所" value={exchange} onChange={e=>{setExchange(e.target.value);setSearch("");}}>{["SHFE","DCE","CZCE","CFFEX","INE","GFEX"].map(e=><option key={e}>{e}</option>)}</select>
        <input aria-label="搜索合约" placeholder="搜索名称或代码" value={search} onChange={e=>setSearch(e.target.value)}/>
        <select aria-label="合约品种" value={product} onChange={e=>setProduct(e.target.value)}><option value="">全部品种</option>{[...new Set(choices.map(c=>c.product))].map(p=><option key={p}>{p}</option>)}</select>
      </div>
      <div className="dashboard-controls"><span>已选 {draft.length}/50</span><span className="panel-spacer"/><button disabled={loading||catalog?.state==="loading"} onClick={()=>void refresh()}>重新获取</button></div>
      {loading?<p role="status">正在读取未到期合约…</p>:loadError?<p role="alert">{loadError}</p>:catalog&&<>
        <p className="dashboard-caption">{catalog.as_of} · {catalog.source??"合约资料"} · {catalog.observed_at?`资料更新 ${new Date(catalog.observed_at).toLocaleString("zh-CN")}`:"尚未获取"}</p>
        <p role="status">{catalog.detail}</p>
        {choices.length>0?<>
          <div className="contract-picker-list" role="group" aria-label="未到期合约">
            {filtered.map(c=><label key={key(c)} className="contract-picker-row">
              <input type="checkbox" aria-label={`选择 ${c.name||c.symbol}`} checked={selected.has(key(c))} disabled={!selected.has(key(c))&&draft.length>=50} onChange={()=>toggle(c)}/>
              <span><strong>{c.name||c.symbol}</strong></span><small>最后交易日 {c.last_trade_on}</small>
            </label>)}
            {!filtered.length&&<p>{choices.length?"没有匹配的合约，请调整搜索或品种筛选。":"当前没有未到期合约。"}</p>}
          </div>
          <p className="dashboard-caption">到期合约会自动移出自选；最后交易日当天保留。不代表此刻正在交易。</p>
        </>:catalog.state==="ready"?<p>当前来源没有返回该交易所的未到期合约。</p>:null}
      </>}
      {(!loading&&(loadError||catalog?.state!=="ready"))&&<button onClick={()=>{close();openConnection();}}>配置账户连接</button>}
      {draft.length>0&&<details><summary>已选合约（{draft.length}）</summary><ul className="dashboard-list">{draft.map(s=><li key={key(s)}><div><span>{names[key(s)]||s.symbol}{s.exchange===exchange&&catalog?.state==="ready"&&!choices.some(c=>key(c)===key(s))?" · 不在当前目录":""}</span><button aria-label={`移除${names[key(s)]||s.symbol}`} onClick={()=>setDraft(draft.filter(i=>key(i)!==key(s)))}>移除</button></div></li>)}</ul></details>}
      <div className="dashboard-controls"><button onClick={()=>void save()}>保存自选</button></div>
    </fieldset>
  </dialog>;
}
