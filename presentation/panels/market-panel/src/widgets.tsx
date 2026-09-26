import { Empty } from "@asterion/ui-kit/WidgetEmpty";
import { dashboardContribution } from "@asterion/ui-overview/public";
import { RealtimeQuotes } from "./RealtimeQuotes";
export const widgets = [
  dashboardContribution({ id: "market.quotes", column: "primary", title: "实时行情", category: "行情", description: "自选合约的实时报价与行情时间", width: 2, defaultVisible: true, render: c => <RealtimeQuotes api={c.marketApi} /> }),
  dashboardContribution({ id: "market.heatmap", title: "板块热力图", category: "行情", description: "期货板块表现：黑色、有色、能源、化工和农产品", width: 2, render: c => <><div className="sector-placeholders">{["黑色", "有色", "能源", "化工", "农产品", "贵金属"].map(s => <div key={s}><strong>{s}</strong><span>—</span></div>)}</div><Empty action={<button onClick={c.openData}>查看数据来源 ↗</button>}>缺少统一交易日的板块成分与涨跌基准，暂不计算板块表现。</Empty></> }),
];
