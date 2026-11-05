import { useState, useSyncExternalStore } from "react";
import { getLocale, translate, useLocale } from "../i18n";
import {
  BackendError,
  CopyDiagnostic,
  asDisplayError,
  errorDiagnostic,
  errorSummary,
  type DisplayError,
} from "../i18n/errors";
import { LocalLogsButton } from "./components/LocalLogsButton";
import "./notifications.css";

type Severity = "error" | "warning";
type Entry = {
  id: number;
  at: Date;
  source: string;
  severity: Severity;
  error: DisplayError;
  namespace: string;
  // The same message repeated in a row is one entry.
  count: number;
};
const identity = (error: DisplayError) =>
  error instanceof BackendError
    ? `${error.code}: ${error.diagnostic}`
    : error instanceof Error
      ? error.message
      : error;

// Where a message comes from, by the namespace its text is translated in.
const sources: Record<string, string> = {
  host: "Terminal",
  "asterion.terminal.research": "研究",
  "asterion.terminal.trading": "交易",
  "asterion.terminal.futures-market": "行情",
  "asterion.terminal.data-workbench": "数据",
};
// A Terminal left open for days keeps the latest ones; the services keep the records.
const KEPT = 200;

let entries: Entry[] = [];
let seen = 0;
let next = 0;
const listeners = new Set<() => void>();
const publish = () => {
  entries = [...entries];
  listeners.forEach(listener => listener());
};

// Reports an error or a warning of this Terminal run to the notification
// centre in the status bar. Nothing is kept beyond the run.
export function notify(error: unknown, options: { namespace?: string; severity?: Severity } = {}) {
  const namespace = options.namespace ?? "host";
  const severity = options.severity ?? "error";
  const shown = asDisplayError(error);
  const last = entries.at(-1);
  if (
    last &&
    last.namespace === namespace &&
    last.severity === severity &&
    identity(last.error) === identity(shown)
  )
    entries[entries.length - 1] = { ...last, id: ++next, at: new Date(), count: last.count + 1 };
  else
    entries.push({
      id: ++next,
      at: new Date(),
      source: sources[namespace] ?? "Terminal",
      severity,
      error: shown,
      namespace,
      count: 1,
    });
  entries.splice(0, Math.max(0, entries.length - KEPT));
  publish();
}

function useEntries() {
  return useSyncExternalStore(
    listener => {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
    () => entries,
  );
}

// The status bar shows the latest message and how many are unread, or just
// "通知" when there is none; opened, it lists them all and opens the log
// directory. An unread trading error stays highlighted until then.
export function NotificationCenter() {
  useLocale();
  const all = useEntries();
  const [open, setOpen] = useState(false);
  const latest = all.at(-1);
  const unread = all.filter(entry => entry.id > seen);
  const urgent = unread.some(entry => entry.source === "交易" && entry.severity === "error");
  const toggle = () => {
    if (!open) seen = latest?.id ?? seen;
    setOpen(!open);
  };
  return (
    <div className="notification-center">
      {open && (
        <section className="notification-panel" aria-label={translate("host", "通知")}>
          <header>
            <strong>{translate("host", "通知")}</strong>
            <span className="panel-spacer" />
            <LocalLogsButton />
            <button
              type="button"
              disabled={!all.length}
              onClick={() => {
                entries = [];
                publish();
              }}
            >
              {translate("host", "清空")}
            </button>
            <button type="button" aria-label={translate("host", "关闭")} onClick={toggle}>
              ×
            </button>
          </header>
          {all.length ? (
            <ol>
              {[...all].reverse().map(entry => (
                <li key={entry.id} className={entry.severity}>
                  <time>{entry.at.toLocaleTimeString(getLocale())}</time>
                  <span className="notification-source">{translate("host", entry.source)}</span>
                  <span className="notification-text">
                    {errorSummary(entry.error, entry.namespace)}
                    {entry.count > 1 && ` ×${entry.count}`}
                  </span>
                  <CopyDiagnostic diagnostic={errorDiagnostic(entry.error, entry.namespace)} />
                </li>
              ))}
            </ol>
          ) : (
            <p className="subtle">{translate("host", "没有消息")}</p>
          )}
        </section>
      )}
      {latest ? (
        <button
          type="button"
          className={`notification-latest ${latest.severity}${urgent ? " urgent" : ""}`}
          aria-expanded={open}
          onClick={toggle}
        >
          <span className="notification-text">{errorSummary(latest.error, latest.namespace)}</span>
          {unread.length > 0 && <span className="notification-count">{unread.length}</span>}
        </button>
      ) : (
        <button type="button" className="notification-none" aria-expanded={open} onClick={toggle}>
          {translate("host", "通知")}
        </button>
      )}
    </div>
  );
}
