import { ErrorCode, type Error as Error_ } from "../generated/asterion/common_pb";
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
// A service's common.Error as a BackendError; codes are ErrorCode's names in lower case.
export function remoteError(error: Error_) {
  return new BackendError(
    ErrorCode[error.code].replace(/^ERROR_CODE_/, "").toLowerCase(),
    error.message,
  );
}
const errors: Record<string, string> = {
  invalid_request: "请求无效，请检查输入",
  unavailable: "服务暂不可用",
  service_restarted: "Lab 意外退出，已自动重启。",
  trading_stalled: "交易服务有一部分处理超过 30 秒没有进展，请查看交易主机的日志。",
  strategy_instance: "策略实例失败或放弃了一个批次，详情见交易页的实例事件。",
  risk_breached: "账户触发硬风控，已暂停新增开仓。",
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
export const prefixedDiagnostics = [
  ["CTP catalog not initialized: ", "CTP catalog not initialized"],
  ["CTP catalog front inactive: ", "CTP catalog front inactive"],
  ["CTP catalog disconnected: ", "CTP catalog disconnected"],
  ["CTP catalog instrument query timed out after ", "CTP catalog instrument query timed out"],
  ["a module of this name already exists: ", "a module of this name already exists"],
  ["a built-in module cannot be changed: ", "a built-in module cannot be changed"],
  ["a built-in module cannot be deleted: ", "a built-in module cannot be deleted"],
  ["a module is strategies/<name>.py or factors/<name>.py: ", "invalid module name"],
  ["feature expression at byte ", "feature expression validation failed"],
  ["cannot read node configuration template: ", "cannot read node configuration template"],
  ["bundled plugin is required: ", "bundled plugin is required"],
  [
    "no month contract of the series has data on ",
    "dominant series has no tradable month contract",
  ],
  ["the outgoing month has no bars from the roll day on: ", "outgoing roll month has no bars"],
  [
    "the roll lacks both settlement prices on the previous day: ",
    "roll lacks previous-day settlement prices",
  ],
  ["invalid roll adjustment ratio on ", "invalid roll adjustment ratio"],
  [
    "a position remains in a contract on a day it has no settlement price: ",
    "open position lacks daily settlement price",
  ],
  ["Tushare did not accept the token: ", "Tushare token rejected"],
  ["the Tushare account has no permission for this interface: ", "Tushare permission denied"],
  ["the Tushare account reached its request limit: ", "Tushare request limit reached"],
  ["Tushare reported an error: ", "Tushare reported an error"],
  ["too many partitions were rejected; the first: ", "too many partitions were rejected"],
  ["reference data was rejected: ", "reference data was rejected"],
  ["the source returned no reference data: ", "source returned no reference data"],
  ["reference data is not stored: ", "reference data is not stored"],
  ["the contract list has no listing date: ", "contract list has no listing date"],
  ["the source does not provide ", "source does not provide requested data"],
  ["the calendar of the range is not stored: ", "calendar of range is not stored"],
  [
    "futures.daily is missing trading days of the range: ",
    "daily bars missing trading days in range",
  ],
  [
    "the contract multiplier does not match the turnover of ",
    "contract multiplier mismatches turnover",
  ],
  ["the contract table of ", "contract table not stored"],
  ["the contract table has no ", "contract missing from contract table"],
  ["no contract list of ", "contract list not stored"],
  ["the contract list does not list ", "contract missing from contract list"],
  ["the contract list gives no listing days of ", "contract list gives no listing days"],
  ["the contract table's units of ", "contract table units unreadable"],
  ["missing price tick of ", "missing historical price tick"],
  ["conflicting price tick of ", "conflicting historical price ticks"],
  ["invalid price tick of ", "price tick must be positive"],
  ["invalid price tick interval", "invalid price tick interval"],
  ["invalid exchange price_ticks file: ", "invalid historical price tick source data"],
  ["invalid Guotai price tick report: ", "invalid historical price tick source data"],
  ["invalid decimal: ", "invalid decimal"],
  ["invalid working order limit: ", "invalid working order limit"],
  ["invalid daily cancel limit: ", "invalid daily cancel limit"],
  ["futures.daily has no settlement price for ", "daily bars lack settlement price"],
  ["instruments cannot be read: ", "instruments cannot be read"],
] as const;

const ctpReasons: Record<number, string> = {
  [-1000]: "CTP market SDK failed",
  [-1]: "CTP network disconnected",
  [-2]: "CTP requests pending",
  [-3]: "CTP request rate exceeded",
  3: "CTP login rejected",
  7: "CTP not initialized",
  8: "CTP front inactive",
  63: "CTP authentication rejected",
  4097: "CTP network read failed",
  4098: "CTP network write failed",
  8193: "CTP receive heartbeat timed out",
  8194: "CTP send heartbeat failed",
  8195: "CTP invalid packet received",
};
export function ctpErrorSummary(code: number, detail = "") {
  const reason = ctpReasons[code];
  return `${reason ? translate("diagnostics", reason) : detail || translate("diagnostics", "CTP request rejected")} (CTP ${code})`;
}
// The localized form of a known English service diagnostic.
export function diagnosticSummary(message: string) {
  const body = message.replace(/^(?:tushare|shfe|ine|czce|dce|cffex|gfex|gtja|sina): /, "");
  const known = localizeText("diagnostics", body);
  if (known) return known;
  const feed = body.match(/^CTP market rejected: (-?\d+)$/);
  if (feed) return ctpErrorSummary(Number(feed[1]));
  const subscriptions = body.match(/^CTP subscriptions failed: (\d+)$/);
  if (subscriptions)
    return translate("diagnostics", "CTP subscriptions failed", {
      count: Number(subscriptions[1]),
    });
  const ctp = body.match(
    /^CTP catalog (authentication|login|product query|instrument query) rejected: (-?\d+)(?:: (.*))?$/,
  );
  if (ctp)
    return `${translate("diagnostics", `CTP catalog ${ctp[1]} rejected`)}: ${ctpErrorSummary(Number(ctp[2]), ctp[3])}`;
  const match = prefixedDiagnostics.find(([prefix]) => body.startsWith(prefix));
  return match ? translate("diagnostics", match[1]) : undefined;
}
export function asDisplayError(value: unknown): DisplayError {
  return value instanceof Error ? value : String(value);
}
// What a person reads of an error: a known message in the current language.
export function errorSummary(error: DisplayError, namespace = "host") {
  const text = error instanceof Error ? error.message : error;
  if (error instanceof BackendError)
    return (
      diagnosticSummary(error.diagnostic) ??
      [translate("host", errors[error.code] ?? errors.operation_failed), error.diagnostic]
        .filter(Boolean)
        .join(": ")
    );
  const known = localizeText(namespace, text) ?? localizeText("host", text);
  if (known) return known;
  return getLocale() === "en-US" && /\p{Script=Han}/u.test(text)
    ? translate("host", errors.operation_failed)
    : text;
}

// What a report of the error needs beyond its summary, or nothing.
export function errorDiagnostic(error: DisplayError, namespace = "host") {
  if (error instanceof BackendError) return `${error.code}: ${error.diagnostic}`;
  const text = error instanceof Error ? error.message : error;
  if ((localizeText(namespace, text) ?? localizeText("host", text)) !== undefined) return "";
  return errorSummary(error, namespace) === text ? "" : text;
}

// An in-flight background read whose saved service was cleared or replaced.
export const isDeselectedService = (error: unknown) =>
  error instanceof BackendError &&
  (error.diagnostic === "no Market is configured" ||
    error.diagnostic === "no Warehouse is configured" ||
    error.diagnostic === "trading connection is unavailable" ||
    error.diagnostic === "Market address changed during connection" ||
    error.diagnostic.startsWith("service selection changed during operation") ||
    error.diagnostic.startsWith("trading connection changed"));

// Shared presentation for inline validation and notification details.
export function ErrorNotice({
  error,
  namespace = "host",
}: {
  error: DisplayError;
  namespace?: string;
}) {
  useLocale();
  if (!error) return null;
  const diagnostic = errorDiagnostic(error, namespace);
  return (
    <span className="error-notice" title={diagnostic || undefined}>
      {errorSummary(error, namespace)}
    </span>
  );
}
