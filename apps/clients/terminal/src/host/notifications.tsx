import { useEffect, useId, useRef, useState, useSyncExternalStore } from "react";
import { translate, useLocale } from "../i18n";
import {
  BackendError,
  ErrorNotice,
  asDisplayError,
  errorSummary,
  isDeselectedService,
  type DisplayError,
} from "../i18n/errors";
import { Icon } from "../ui/Icon";
import { LocalLogsButton } from "./components/LocalLogsButton";
import "./notifications.css";

type Severity = "error" | "warning";
type ForwardedNotification = {
  message: string;
  code?: string;
  namespace: string;
  severity: Severity;
};
const channelName = "asterion.notifications";
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
  if (new URLSearchParams(location.search).get("screen") === "settings") {
    const channel = new BroadcastChannel(channelName);
    channel.postMessage({
      message: shown instanceof Error ? shown.message : shown,
      code: shown instanceof BackendError ? shown.code : undefined,
      namespace,
      severity,
    } satisfies ForwardedNotification);
    channel.close();
    return;
  }
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

// undefined means a read or reconnect is pending; only a successful read clears the failure.
export function useErrorNotification(error: DisplayError | undefined, namespace = "host") {
  const reported = useRef("");
  useEffect(() => {
    if (error === undefined || isDeselectedService(error)) return;
    const key = error ? `${namespace}: ${identity(error)}` : "";
    if (key && key !== reported.current) notify(error, { namespace });
    reported.current = key;
  }, [error, namespace]);
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

// The status bar shows the latest unread message and unread count; once read it
// shows "通知" in red with the errors left, or in yellow with the warnings left,
// until cleared. The panel lists the session's messages and opens the log
// directory. An unread trading error stays highlighted until opened.
export function NotificationCenter() {
  useLocale();
  useEffect(() => {
    const channel = new BroadcastChannel(channelName);
    channel.onmessage = ({ data }: MessageEvent<ForwardedNotification>) => {
      const error =
        data.code === undefined ? data.message : new BackendError(data.code, data.message);
      notify(error, { namespace: data.namespace, severity: data.severity });
    };
    return () => channel.close();
  }, []);
  const panelId = useId();
  const anchor = `--notifications-${panelId.replace(/:/g, "")}`;
  const trigger = useRef<HTMLButtonElement>(null);
  const all = useEntries();
  const [open, setOpen] = useState(false);
  const latest = all.at(-1);
  useEffect(() => {
    if (open && latest && latest.id > seen) seen = latest.id;
  }, [open, latest]);
  const effectiveSeen = open && latest && latest.id > seen ? latest.id : seen;
  const unread = all.filter(entry => entry.id > effectiveSeen);
  const latestUnread = !open ? unread.at(-1) : undefined;
  const urgent = unread.some(entry => entry.source === "交易" && entry.severity === "error");
  const errors = all.filter(entry => entry.severity === "error").length;
  const tone = errors ? "error" : "warning";
  return (
    <div className="notification-center">
      <section
        id={panelId}
        popover="auto"
        className="notification-panel"
        aria-label={translate("host", "通知")}
        style={{ positionAnchor: anchor }}
        onToggle={event => {
          const expanded = event.newState === "open";
          if (expanded) seen = latest?.id ?? seen;
          setOpen(expanded);
        }}
      >
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
          <button
            type="button"
            aria-label={translate("host", "关闭")}
            {...{ commandfor: panelId, command: "hide-popover" }}
            onClick={() => trigger.current?.focus()}
          >
            <Icon name="close" size={14} />
          </button>
        </header>
        {open &&
          (all.length ? (
            <ol>
              {[...all].reverse().map(entry => (
                <li key={entry.id} className={entry.severity}>
                  <div className="notification-meta">
                    <span className="notification-source">{translate("host", entry.source)}</span>
                    <time>
                      {entry.at.toLocaleTimeString("sv-SE", {
                        timeZone: "Asia/Shanghai",
                        hour12: false,
                      })}
                    </time>
                  </div>
                  <span className="notification-text">
                    <ErrorNotice error={entry.error} namespace={entry.namespace} />
                    {entry.count > 1 && ` ×${entry.count}`}
                  </span>
                </li>
              ))}
            </ol>
          ) : (
            <p className="subtle">{translate("host", "没有消息")}</p>
          ))}
      </section>
      <button
        ref={trigger}
        type="button"
        className={
          latestUnread
            ? `notification-latest ${tone}${urgent ? " urgent" : ""}`
            : all.length
              ? `notification-kept ${tone}`
              : "notification-none"
        }
        style={{ anchorName: anchor }}
        {...{ commandfor: panelId, command: "toggle-popover" }}
        aria-expanded={open}
      >
        {latestUnread ? (
          <>
            <span className="notification-text">
              {errorSummary(latestUnread.error, latestUnread.namespace)}
            </span>
            <span className="notification-count">{unread.length}</span>
          </>
        ) : (
          <>
            {translate("host", "通知")}
            {all.length > 0 && <span className="notification-count">{errors || all.length}</span>}
          </>
        )}
      </button>
    </div>
  );
}
