import { DashboardTable } from "../overview/DashboardTable";
import { dashboardContribution } from "../overview/public";

export const positionColumns = ["合约", "方向", "手数", "今仓", "昨仓", "持仓均价", "浮动盈亏"].map((title, i) => ({ title, numeric: i > 1 }));

export const widgets = [
  dashboardContribution({
    id: "trading.portfolio", title: "账户与持仓", description: "资金摘要与逐合约持仓", category: "账户与风险", width: 2, defaultVisible: true, column: "secondary",
    render: c => <>
      <dl className="account-summary" aria-label="资金摘要">
        {["账户权益", "可用资金", "保证金占用", "当日盈亏"].map(label => <div key={label}><dt>{label}</dt><dd>—</dd></div>)}
      </dl>
      <DashboardTable label="持仓明细" columns={positionColumns} rows={[]} empty="持仓数据暂不可用" />
      <div className="account-connection"><span>交易账户尚未接入 · 金额与持仓未知</span><button onClick={c.openTrading}>查看接入状态 ↗</button></div>
    </>,
  }),
  dashboardContribution({
    id: "trading.risk-metrics", title: "风险概览", description: "账户敞口、集中度及风险走势", category: "账户与风险", width: 2, defaultVisible: true, column: "secondary",
    render: () => <>
      <dl className="account-summary risk-summary" aria-label="风险指标">
        {["账户敞口", "持仓集中度", "风险状态"].map(label => <div key={label}><dt>{label}</dt><dd>—</dd></div>)}
      </dl>
      <p className="dashboard-caption">等待账户、持仓与合约规则数据，暂不能评估风险。</p>
    </>,
  }),
];
