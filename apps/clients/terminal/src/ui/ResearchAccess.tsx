import { useEffect, useRef, useState, type ReactNode } from "react";
import { translate } from "../i18n";
import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
import type { TerminalContext } from "../../plugins/contract";
const t = (key: string) => translate("host", key);

// Entering a local research workflow prepares its service once. A remote
// connection is never replaced and failed attempts require an explicit retry.
export function ResearchAccess({
  context,
  enabled = true,
  children,
}: {
  context: TerminalContext;
  enabled?: boolean;
  children: (context: TerminalContext) => ReactNode;
}) {
  const { snapshot, busy, trade } = context;
  const research = snapshot?.research;
  const attempted = useRef<string | null>(null);
  const [preparing, setPreparing] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  const identity = research?.connection_id ?? "local";
  function prepare() {
    attempted.current = identity;
    setPreparing(true);
    setError("");
    void trade("research.local")
      .catch(reason => setError(asDisplayError(reason)))
      .finally(() => setPreparing(false));
  }
  useEffect(() => {
    if (enabled && research?.online) attempted.current = identity;
    if (
      enabled &&
      snapshot &&
      !snapshot.stale &&
      !busy &&
      !research?.online &&
      !research?.remote &&
      attempted.current !== identity
    )
      prepare();
    // Admission is keyed by the selected service, not by render callback identity.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [enabled, snapshot, busy, research?.online, research?.remote, identity]);
  const unavailable = enabled && !research?.online;
  return (
    <>
      {unavailable && (
        <div className="research-access" role="status">
          <span>
            {t(
              research?.remote
                ? "远程研究连接已断开，当前运行位置保持不变。"
                : preparing || attempted.current === null
                  ? "正在准备研究环境…"
                  : "研究环境未就绪。",
            )}
          </span>
          {error && <ErrorNotice error={error} />}
          {research?.remote ? (
            <button onClick={() => context.openSettings("connections")}>{t("查看连接")}</button>
          ) : (
            !preparing &&
            attempted.current === identity && (
              <button disabled={busy || preparing} onClick={prepare}>
                {t("重试准备")}
              </button>
            )
          )}
        </div>
      )}
      {children({ ...context, busy: busy || preparing })}
    </>
  );
}
