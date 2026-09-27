import { useState } from "react";
import { translate, type TerminalContext } from "../contract";
import { MarketPanel } from "./MarketPanel";
import { LivePanel } from "./LivePanel";
const t=(key:string)=>translate("asterion.terminal.futures-market",key);
export function Workspace(context: TerminalContext) {
  const [mode,setMode]=useState(context.marketMode ?? "live");
  return <div className="market-workspace"><div className="market-modes" role="tablist" aria-label={t("行情来源")}><button role="tab" aria-selected={mode==="live"} onClick={()=>setMode("live")}>{t("实时行情")}</button><button role="tab" aria-selected={mode==="history"} onClick={()=>setMode("history")}>{t("历史行情")}</button></div>{mode==="live" ? <LivePanel context={context}/> : <MarketPanel data={context.snapshot?.dataset ?? null} openData={()=>context.navigate("workspace.data")}/>}</div>;
}
