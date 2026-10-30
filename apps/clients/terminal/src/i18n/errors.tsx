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
  ["feature expression at byte ", "feature expression validation failed"],
  ["cannot read node configuration template: ", "cannot read node configuration template"],
  [
    "bundled plugin is required: ",
    "\u5e94\u7528\u81ea\u5e26\u7684\u63d2\u4ef6\u5fc5\u987b\u4fdd\u6301\u542f\u7528",
  ],
  [
    "no month contract of the series has data on ",
    "\u4e3b\u529b\u8fde\u7eed\u5728\u67d0\u4e2a\u4ea4\u6613\u65e5\u6ca1\u6709\u53ef\u4ee5\u6301\u6709\u7684\u6708\u4efd\uff1a\u90a3\u5929\u6ca1\u6709\u6708\u4efd\u6709\u7ed3\u7b97\u4ef7\u548c\u524d\u4e00\u4ea4\u6613\u65e5\u7684\u6301\u4ed3\u91cf\uff0c\u6216\u53ea\u6709\u5f53\u5929\u5230\u671f\u7684\u6708\u4efd\u6709",
  ],
  [
    "the outgoing month has no bars from the roll day on: ",
    "\u88ab\u6362\u4e0b\u7684\u6708\u4efd\u4ece\u6362\u6708\u65e5\u8d77\u518d\u6ca1\u6709 K \u7ebf\uff0c\u6301\u4ed3\u65e0\u6cd5\u5e73\u6389\uff1a\u5206\u949f\u7ebf\u8bf7\u8865\u9f50\u8be5\u6708\u4efd\u7684\u4e0b\u8f7d\uff1b\u65e5\u7ebf\u8868\u793a\u5b83\u6b64\u540e\u6ca1\u6709\u6210\u4ea4",
  ],
  [
    "the roll lacks both settlement prices on the previous day: ",
    "\u6362\u6708\u524d\u4e00\u4ea4\u6613\u65e5\u7f3a\u5c11\u65b0\u65e7\u5408\u7ea6\u7684\u7ed3\u7b97\u4ef7\uff0c\u65e0\u6cd5\u8ba1\u7b97\u590d\u6743\u7cfb\u6570",
  ],
  ["invalid roll adjustment ratio on ", "\u6362\u6708\u7684\u590d\u6743\u7cfb\u6570\u65e0\u6548"],
  [
    "a position remains in a contract on a day it has no settlement price: ",
    "\u4e00\u4e2a\u6708\u4efd\u5408\u7ea6\u7684\u6301\u4ed3\u7559\u5230\u4e86\u5b83\u6ca1\u6709\u7ed3\u7b97\u4ef7\u7684\u4ea4\u6613\u65e5\uff1a\u6362\u6708\u540e\u6210\u4ea4\u91cf\u4e0d\u8db3\u6ca1\u6709\u5e73\u5b8c\uff0c\u6216\u6570\u636e\u6e90\u7f3a\u5b83\u8fd9\u4e00\u5929\u7684\u7ed3\u7b97\u4ef7",
  ],
  [
    "Tushare did not accept the token: ",
    "Tushare \u6ca1\u6709\u63a5\u53d7\u8be5 Token\uff0c\u8bf7\u68c0\u67e5\u540e\u91cd\u65b0\u4fdd\u5b58",
  ],
  [
    "the Tushare account has no permission for this interface: ",
    "Tushare \u8d26\u53f7\u6ca1\u6709\u8be5\u63a5\u53e3\u7684\u6743\u9650",
  ],
  [
    "the Tushare account reached its request limit: ",
    "Tushare \u8d26\u53f7\u7684\u8bf7\u6c42\u989d\u5ea6\u5df2\u7528\u5c3d\uff0c\u8bf7\u7a0d\u540e\u518d\u8bd5\u6216\u964d\u4f4e\u6bcf\u5206\u949f\u8bf7\u6c42\u6570",
  ],
  ["Tushare reported an error: ", "Tushare \u8fd4\u56de\u4e86\u9519\u8bef"],
  [
    "too many partitions were rejected; the first: ",
    "\u88ab\u62d2\u7edd\u7684\u5206\u533a\u8fc7\u591a\uff0c\u66f4\u65b0\u5df2\u505c\u6b62\uff1b\u8be6\u60c5\u91cc\u662f\u7b2c\u4e00\u6761\u539f\u56e0",
  ],
  [
    "reference data was rejected: ",
    "\u5408\u7ea6\u8868\u6216\u4ea4\u6613\u65e5\u5386\u672a\u901a\u8fc7\u6821\u9a8c\uff0c\u4f9d\u8d56\u5b83\u7684\u4ea4\u6613\u65e5\u6ca1\u6709\u4e0b\u8f7d",
  ],
  [
    "the source returned no reference data: ",
    "\u6570\u636e\u6e90\u6ca1\u6709\u8fd4\u56de\u5408\u7ea6\u8868\u6216\u4ea4\u6613\u65e5\u5386\uff0c\u4f9d\u8d56\u5b83\u7684\u4ea4\u6613\u65e5\u6ca1\u6709\u4e0b\u8f7d",
  ],
  [
    "reference data is not stored: ",
    "\u5408\u7ea6\u8868\u6216\u4ea4\u6613\u65e5\u5386\u5c1a\u672a\u5165\u5e93\uff0c\u4f9d\u8d56\u5b83\u7684\u4ea4\u6613\u65e5\u6ca1\u6709\u4e0b\u8f7d",
  ],
  [
    "the contract list has no listing date: ",
    "\u5408\u7ea6\u8868\u91cc\u6ca1\u6709\u4e0a\u5e02\u65e5\u671f\uff0c\u65e0\u6cd5\u786e\u5b9a\u8be5\u4ea4\u6613\u6240\u7684\u8d77\u59cb\u65e5",
  ],
  [
    "the source does not provide ",
    "\u8be5\u6570\u636e\u6e90\u4e0d\u63d0\u4f9b\u8fd9\u5f20\u6570\u636e\u8868\u6216\u8fd9\u4e2a\u4ea4\u6613\u6240",
  ],
  [
    "the calendar of the range is not stored: ",
    "\u8303\u56f4\u5185\u6709\u4e00\u5e74\u7684\u4ea4\u6613\u65e5\u5386\u6ca1\u6709\u5165\u5e93\u3002\u4ea4\u6613\u6240\u5728\u90a3\u4e00\u5e74\u8fd8\u6ca1\u6709\u6210\u7acb\u65f6\uff0c\u628a\u8d77\u59cb\u65e5\u671f\u6539\u5230\u5b83\u6210\u7acb\u4e4b\u540e\uff1b\u5426\u5219\u5148\u66f4\u65b0\u5168\u5e02\u573a\u6570\u636e",
  ],
  [
    "futures.daily is missing trading days of the range: ",
    "\u5168\u5e02\u573a\u65e5\u7ebf\u8868\u7f3a\u5c11\u8303\u56f4\u5185\u7684\u4ea4\u6613\u65e5\uff0c\u8bf7\u5148\u8865\u9f50\u518d\u63d0\u4ea4\uff1b\u8be6\u60c5\u91cc\u662f\u4ea4\u6613\u6240\u548c\u65e5\u671f",
  ],
  [
    "the contract multiplier does not match the turnover of ",
    "\u5408\u7ea6\u8868\u91cc\u7684\u5408\u7ea6\u4e58\u6570\u4e0e\u65e5\u7ebf\u8868\u91cc\u8fd9\u4e2a\u5408\u7ea6\u7684\u6210\u4ea4\u989d\u5bf9\u4e0d\u4e0a\uff08\u5408\u7ea6\u6539\u8fc7\u4ea4\u6613\u5355\u4f4d\u65f6\uff0c\u5408\u7ea6\u8868\u53ea\u6709\u73b0\u5728\u7684\u503c\uff09\uff1b\u8be6\u60c5\u91cc\u662f\u5408\u7ea6\u548c\u6309\u6210\u4ea4\u989d\u63a8\u51fa\u7684\u4e58\u6570",
  ],
  [
    "the contract table of ",
    "\u8fd9\u4e2a\u4ea4\u6613\u6240\u7684\u5408\u7ea6\u8868\u8fd8\u6ca1\u6709\u5165\u5e93\uff0c\u7b97\u4e0d\u51fa\u5408\u7ea6\u4e58\u6570\u3002\u8bf7\u5148\u5728\u300c\u6570\u636e \u2192 \u5168\u5e02\u573a\u6570\u636e\u300d\u66f4\u65b0",
  ],
  [
    "the contract table has no ",
    "\u5408\u7ea6\u8868\u91cc\u6ca1\u6709\u8fd9\u4e2a\u5408\u7ea6\uff0c\u7b97\u4e0d\u51fa\u5b83\u7684\u5408\u7ea6\u4e58\u6570\uff1b\u8be6\u60c5\u91cc\u662f\u5408\u7ea6",
  ],
  [
    "no contract list of ",
    "\u8fd9\u4e2a\u4ea4\u6613\u6240\u7684\u5408\u7ea6\u8868\u8fd8\u6ca1\u6709\u5165\u5e93\uff0c\u4e0d\u77e5\u9053\u5408\u7ea6\u5728\u54ea\u4e9b\u4ea4\u6613\u65e5\u4e0a\u5e02\uff0c\u753b\u4e0d\u51fa\u65e5\u7ebf\u3002\u8bf7\u5148\u5728\u300c\u6570\u636e \u2192 \u5168\u5e02\u573a\u6570\u636e\u300d\u66f4\u65b0",
  ],
  [
    "the contract list does not list ",
    "\u5408\u7ea6\u8868\u91cc\u6ca1\u6709\u8fd9\u4e2a\u5408\u7ea6\uff0c\u753b\u4e0d\u51fa\u5b83\u7684\u65e5\u7ebf\uff1a\u5408\u7ea6\u8868\u53ef\u80fd\u65e9\u4e8e\u5b83\u4e0a\u5e02\uff0c\u8bf7\u5148\u5728\u300c\u6570\u636e \u2192 \u5168\u5e02\u573a\u6570\u636e\u300d\u66f4\u65b0\u5408\u7ea6\u8868\uff1b\u8be6\u60c5\u91cc\u662f\u5408\u7ea6",
  ],
  [
    "the contract list gives no listing days of ",
    "\u5408\u7ea6\u8868\u6ca1\u6709\u7ed9\u51fa\u8fd9\u4e2a\u5408\u7ea6\u7684\u4e0a\u5e02\u548c\u9000\u5e02\u65e5\u671f\uff0c\u753b\u4e0d\u51fa\u5b83\u7684\u65e5\u7ebf\uff1b\u8be6\u60c5\u91cc\u662f\u5408\u7ea6",
  ],
  [
    "the contract table's units of ",
    "\u5408\u7ea6\u8868\u91cc\u8fd9\u4e2a\u5408\u7ea6\u7684\u4ea4\u6613\u5355\u4f4d\u548c\u62a5\u4ef7\u5355\u4f4d\u8bfb\u4e0d\u61c2\uff0c\u7b97\u4e0d\u51fa\u5408\u7ea6\u4e58\u6570\uff1b\u8be6\u60c5\u91cc\u662f\u5408\u7ea6",
  ],
  [
    "futures.daily has no auction prices of ",
    "\u5168\u5e02\u573a\u65e5\u7ebf\u8868\u91cc\u8fd9\u4e2a\u54c1\u79cd\u5728\u67d0\u4e2a\u4ea4\u6613\u65e5\u6ca1\u6709\u7ade\u4ef7\u6210\u4ea4\u4ef7\uff0c\u63a8\u4e0d\u51fa\u5f53\u5929\u7684\u6700\u5c0f\u53d8\u52a8\u4ef7\u4f4d\uff1b\u8be6\u60c5\u91cc\u662f\u54c1\u79cd\u548c\u65e5\u671f",
  ],
  [
    "futures.daily has no settlement price for ",
    "\u5168\u5e02\u573a\u65e5\u7ebf\u8868\u91cc\u6ca1\u6709\u8be5\u5408\u7ea6\u5728\u67d0\u4e2a\u4ea4\u6613\u65e5\u7684\u7ed3\u7b97\u4ef7\uff1b\u8be6\u60c5\u91cc\u662f\u5408\u7ea6\u548c\u65e5\u671f",
  ],
  [
    "instruments cannot be read: ",
    "\u6240\u9009\u7684\u54c1\u79cd\u6216\u5408\u7ea6\u91cc\u6709\u5728\u8fd9\u4e2a\u8303\u56f4\u5185\u8bfb\u4e0d\u51fa\u7684\uff1b\u8be6\u60c5\u91cc\u9010\u4e2a\u5217\u51fa\u4ee3\u7801\u548c\u539f\u56e0\uff0c\u628a\u5b83\u4eec\u79fb\u9664\u540e\u518d\u63d0\u4ea4",
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
  const immediateDiagnostic =
    error instanceof BackendError &&
    (error.diagnostic.startsWith("service program synchronization failed: ") ||
      error.diagnostic.startsWith("node manager startup failed: "));
  return (
    <span>
      {summary}
      {diagnostic && (
        <>
          {" "}
          {!immediateDiagnostic && (
            <button type="button" aria-expanded={details} onClick={() => setDetails(!details)}>
              {translate("host", "详情")}
            </button>
          )}
          {(details || immediateDiagnostic) && (
            <span className="error-diagnostic">{diagnostic}</span>
          )}
        </>
      )}
    </span>
  );
}
