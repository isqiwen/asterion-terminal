// Test-only entry: never imported by the application or production build.
import { createRoot } from "react-dom/client";
import { Dashboard } from "@asterion/ui-overview/Dashboard";
import { DashboardTable } from "@asterion/ui-overview/DashboardTable";
import { quoteColumns } from "@asterion/ui-market-panel/RealtimeQuotes";
import { positionColumns, widgets as trading } from "@asterion/ui-trading-panel/widgets";
import { widgets as market } from "@asterion/ui-market-panel/widgets";
import "@asterion/ui-kit/theme/style.css";
const count = new URLSearchParams(location.search).get("rows") === "many" ? 80 : 6;
const widgets = [market[0].value, ...trading.map(w => w.value)].map(w => ({ ...w,
  render: w.id === "market.quotes" ? () => <><p className="dashboard-caption">自动化测试数据 · 非真实行情</p><DashboardTable label="实时报价" columns={quoteColumns} rows={Array.from({length:count},(_,i)=>({id:String(i), cells:[`TEST${2600+i}`,"123,456.78","+1.23%","123,456","234,567","09:30:01","更新中"]}))} empty="无报价" /></> : w.id === "trading.portfolio" ? () => <><dl className="account-summary" aria-label="资金摘要">{["账户权益","可用资金","保证金占用","当日盈亏"].map(label=><div key={label}><dt>{label}</dt><dd>123,456.78</dd></div>)}</dl><DashboardTable label="持仓明细" columns={positionColumns} rows={Array.from({length:count},(_,i)=>({id:String(i),cells:[`TEST${2600+i}`,i%2?"多":"空","10","6","4","123,456.78","+1,234.56"]}))} empty="无持仓" /></> : w.render
}));
createRoot(document.getElementById("root")!).render(<Dashboard widgets={widgets} marketApi={{key:"fixture",request:async()=>{throw new Error("unused");},text:async()=>""}} storageKey="test-layout" refresh={()=>{}} openTrading={()=>{}} openIntelligence={()=>{}} openData={()=>{}} openTasks={()=>{}} jobs={[]} taskViews={[]} connected={true} catalogError="" refreshedAt={null} />);
