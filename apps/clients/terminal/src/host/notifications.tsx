import { useEffect, useSyncExternalStore } from "react";
import { translate, useLocale } from "../i18n";
import {
  BackendError,
  CopyDiagnostic,
  asDisplayError,
  errorDiagnostic,
  errorSummary,
  type DisplayError,
} from "../i18n/errors";
import "./notifications.css";

type Notification = { id: number; error: DisplayError; namespace: string; sticky: boolean };

let shown: Notification[] = [];
let next = 0;
const listeners = new Set<() => void>();
const publish = (items: Notification[]) => {
  shown = items;
  listeners.forEach(listener => listener());
};

// Reports a failed operation in the window's one notification area. A sticky
// one (trading) stays until it is closed; others close after 8 s.
export function notify(error: unknown, options: { namespace?: string; sticky?: boolean } = {}) {
  // A missing Market is a state the status bar shows, not a failure to report.
  if (error instanceof BackendError && error.code === "market_unconfigured") return;
  publish([
    ...shown,
    {
      id: ++next,
      error: asDisplayError(error),
      namespace: options.namespace ?? "host",
      sticky: !!options.sticky,
    },
  ]);
}

const dismiss = (id: number) => publish(shown.filter(item => item.id !== id));

function Item({ item }: { item: Notification }) {
  useEffect(() => {
    if (item.sticky) return;
    const timer = window.setTimeout(() => dismiss(item.id), 8000);
    return () => window.clearTimeout(timer);
  }, [item]);
  return (
    <li className={item.sticky ? "notification sticky" : "notification"} role="alert">
      <span>{errorSummary(item.error, item.namespace)}</span>
      <CopyDiagnostic diagnostic={errorDiagnostic(item.error, item.namespace)} />
      <button type="button" aria-label={translate("host", "关闭")} onClick={() => dismiss(item.id)}>
        ×
      </button>
    </li>
  );
}

export function Notifications() {
  useLocale();
  const items = useSyncExternalStore(
    listener => {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
    () => shown,
  );
  if (!items.length) return null;
  return (
    <ul className="notifications" aria-label={translate("host", "通知")}>
      {items.map(item => (
        <Item key={item.id} item={item} />
      ))}
    </ul>
  );
}
