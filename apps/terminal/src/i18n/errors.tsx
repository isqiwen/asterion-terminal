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
      ? (localizeText("diagnostics", error.diagnostic) ??
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
