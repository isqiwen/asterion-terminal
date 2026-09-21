import { Empty } from "../../components/WidgetEmpty";
import { dashboardContribution } from "../overview/public";
export const widgets = [
  ["sentiment", "市场情绪", "情绪指标、计算口径与来源", "尚未接入可解释的情绪来源，不生成综合评分。"],
  ["news", "市场新闻与公告", "交易所公告、规则调整与市场新闻", "尚未接入新闻与交易所公告来源。"],
  ["rates", "利率与信用", "利率、收益率与信用市场摘要", "尚未接入利率与信用数据来源。"],
].map(([id, title, description, reason]) => dashboardContribution({
  id: `intelligence.${id}`, title, description, category: "情报与宏观", width: id === "news" ? 2 : 1,
  render: c => <><div className="widget-unavailable">来源未接入</div><Empty action={<button onClick={c.openIntelligence}>查看情报接入状态 ↗</button>}>{reason}</Empty></>,
}));
