import { useState } from "react";
import { getLocale, localizeText, translate, useLocale } from "./index";
export type DisplayError = string | Error;
export class BackendError extends Error {
  constructor(
    readonly code: string,
    readonly diagnostic: string,
  ) {
    super(diagnostic);
  }
}
const errors: Record<string, string> = {
  invalid_request: "请求无效，请检查输入",
  unavailable: "服务暂不可用",
  conflict: "操作与当前状态冲突",
  permission_denied: "权限不足",
  resource_exhausted: "资源不足",
  cancelled: "操作已取消",
  not_found: "对象不存在",
  recovery_required: "需要恢复后继续",
  internal_error: "内部错误",
  operation_failed: "操作失败",
};
// These messages carry operation-specific evidence after a stable prefix.
// Keep that evidence in Details; do not expose service identifiers as UI copy.
const prefixedDiagnostics = [
  [
    "Agent initialization failed; inspect node logs: ",
    "Agent initialization failed; inspect node logs",
  ],
  [
    "Agent configuration commit failed; inspect node logs: ",
    "Agent configuration commit failed; inspect node logs",
  ],
  [
    "Agent upgrade is waiting for a recoverable service boundary: ",
    "更新正在等待后台工作安全结束，请稍后重试",
  ],
  [
    "Agent upgrade validate: service lacks an automatic upgrade recovery boundary: ",
    "正在运行的服务尚不支持自动更新，请保留当前工作并稍后重试",
  ],
  ["bundled plugin is required: ", "应用自带的插件必须保持启用"],
  [
    "no month contract of the series has data on ",
    "主力连续在某个交易日没有可以持有的月份：分钟线请补齐各月份的下载；日线表示这个品种那天没有月份成交，或只有当天到期的月份成交",
  ],
  [
    "the outgoing month has no bars on the roll day: ",
    "换月当天旧月份合约没有 K 线，无法平仓：分钟线请补齐该月份的下载；日线表示它当天没有成交，或日线表里没有它当天的结算价",
  ],
  [
    "the roll lacks both settlement prices on the previous day: ",
    "换月前一交易日缺少新旧合约的结算价，无法计算复权系数",
  ],
  ["invalid roll adjustment ratio on ", "换月的复权系数无效"],
  [
    "a position remains in a contract on a day it has no settlement price: ",
    "换月后旧月份合约的持仓未能平掉；该月份在换月日成交量不足",
  ],
  ["cannot validate owned regular file ", "durable file ownership or type is invalid"],
  ["cannot prepare durable write ", "cannot prepare durable file contents"],
  ["cannot verify publication identity ", "durable publication source changed"],
  ["Tushare did not accept the token: ", "Tushare 没有接受该 Token，请检查后重新保存"],
  ["the Tushare account has no permission for this interface: ", "Tushare 账号没有该接口的权限"],
  [
    "the Tushare account reached its request limit: ",
    "Tushare 账号的请求额度已用尽，请稍后再试或降低每分钟请求数",
  ],
  ["Tushare reported an error: ", "Tushare 返回了错误"],
  [
    "too many partitions were rejected; the first: ",
    "被拒绝的分区过多，更新已停止；详情里是第一条原因",
  ],
  ["reference data was rejected: ", "合约表或交易日历未通过校验，依赖它的交易日没有下载"],
  [
    "the source returned no reference data: ",
    "数据源没有返回合约表或交易日历，依赖它的交易日没有下载",
  ],
  ["reference data is not stored: ", "合约表或交易日历尚未入库，依赖它的交易日没有下载"],
  ["the contract list has no listing date: ", "合约表里没有上市日期，无法确定该交易所的起始日"],
  ["the source does not provide ", "该数据源不提供这张数据表或这个交易所"],
  [
    "the calendar of the range is not stored: ",
    "范围内有一年的交易日历没有入库。交易所在那一年还没有成立时，把起始日期改到它成立之后；否则先更新全市场数据",
  ],
  [
    "futures.daily is missing trading days of the range: ",
    "全市场日线表缺少范围内的交易日，请先补齐再提交；详情里是交易所和日期",
  ],
  [
    "the contract multiplier does not match the turnover of ",
    "所填的合约乘数与日线表里这个合约的成交额对不上；详情里是合约和按成交额推出的乘数",
  ],
  [
    "futures.daily has no settlement price for ",
    "全市场日线表里没有该合约在某个交易日的结算价；详情里是合约和日期",
  ],
  [
    "instruments cannot be read: ",
    "所选的品种或合约里有在这个范围内读不出的；详情里逐个列出代码和原因，把它们移除后再提交",
  ],
] as const;
// The localized form of a known English service diagnostic.
export function diagnosticSummary(message: string) {
  const known = localizeText("diagnostics", message);
  if (known) return known;
  const match = prefixedDiagnostics.find(([prefix]) => message.startsWith(prefix));
  return match ? translate("diagnostics", match[1]) : undefined;
}
export function asDisplayError(value: unknown): DisplayError {
  return value instanceof Error ? value : String(value);
}
export function ErrorNotice({
  error,
  namespace = "host",
}: {
  error: DisplayError;
  namespace?: string;
}) {
  useLocale();
  const [details, setDetails] = useState(false);
  const text = error instanceof Error ? error.message : error;
  const known = localizeText(namespace, text) ?? localizeText("host", text);
  const summary =
    error instanceof BackendError
      ? (diagnosticSummary(error.diagnostic) ??
        translate("host", errors[error.code] ?? errors.operation_failed))
      : (known ??
        (getLocale() === "en-US" && /\p{Script=Han}/u.test(text)
          ? translate("host", errors.operation_failed)
          : text));
  const diagnostic =
    error instanceof BackendError
      ? `${error.code}: ${error.diagnostic}`
      : known
        ? ""
        : summary === text
          ? ""
          : text;
  return (
    <span>
      {summary}
      {diagnostic && (
        <>
          {" "}
          <button type="button" aria-expanded={details} onClick={() => setDetails(!details)}>
            {translate("host", "详情")}
          </button>
          {details && <span className="error-diagnostic">{diagnostic}</span>}
        </>
      )}
    </span>
  );
}
