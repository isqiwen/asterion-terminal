import type { EvaluationMetricState } from "../../src/bridge/client";
export const evaluationMetricReasons: Record<
  Exclude<EvaluationMetricState, "AVAILABLE">,
  string
> = {
  SOURCE_TIME_NOT_RECORDED: "原观测时刻未记录",
  NON_POSITIVE_OBSERVATION_DURATION: "实际观测时长不为正",
  FEWER_THAN_TWO_COMPLETE_RETURNS: "不足两个完整结算收益周期",
  NON_POSITIVE_EQUITY_PATH: "区间中存在非正权益",
  ANNUAL_PERIODS_NOT_CONFIGURED: "年期数A未配置",
  RISK_FREE_NOT_CONFIGURED: "无风险收益未配置",
  CONSTANT_RETURNS: "收益恒定，夏普分母为零",
  NO_DRAWDOWN: "无回撤，卡玛分母为零",
  NON_FINITE_STATISTIC: "统计值超出有限数值范围",
  FEWER_THAN_TWO_OBSERVATIONS: "不足两个日结算观察",
  GROSS_NOT_RECORDED: "原总手数未记录",
  MARGIN_NOT_RECORDED: "原保证金未记录",
  ZERO_GROSS_NOTIONAL: "所列总名义金额为零，集中度不适用",
  SHORTER_THAN_ONE_YEAR: "观测不足一年，不年化",
};
