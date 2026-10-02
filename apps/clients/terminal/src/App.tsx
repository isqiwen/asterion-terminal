import { createPortal } from "react-dom";
import { Icon } from "./ui/Icon";
import { openSettings, closeSettings, settingsPage, type SettingsPage } from "./settings/window";
import { ServiceStatus } from "./host/components/ServiceStatus";
import { ErrorNotice, asDisplayError, type DisplayError } from "./i18n/errors";
import { translate, type MessageValues } from "./i18n";
const t = (key: string, values?: MessageValues) => translate("host", key, values);
import { useLocale } from "./i18n";
import { SetupGate } from "./startup/SetupGate";
import { Suspense, useCallback, useEffect, useRef, useState } from "react";
import { WindowFrame } from "@asterion/workbench/components/WindowFrame";
import { Workbench } from "@asterion/workbench/workspace/Workbench";
import { usePreferences } from "./ui/preferences";
import {
  request,
  pollSnapshot,
  type Snapshot,
  type TerminalCommand,
} from "@asterion/desktop-bridge/client";
import { Settings } from "./settings/Settings";
import { terminalPlugins, workspaces, workspaceShortcuts } from "./plugins";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "./host/workspace/drafts";
import { scopedContext } from "./host/plugin-registry";
import type { TerminalContext } from "../plugins/contract";
import "./ui/theme/style.css";
import "./terminal.css";
import "./host/workspace/workbench.css";
import "./settings/settings.css";
import "./ui/workflows.css";
// One line for every open CTP account: how many may send orders, how many
// are connected read-only, and how many need attention.
function liveSummary(snapshot: Snapshot | null) {
  const entries = Object.values(snapshot?.live ?? {});
  if (!entries.length) return t("尚未打开交易账户");
  let authorized = 0,
    ready = 0,
    other = 0;
  for (const { session, connection } of entries) {
    if (snapshot?.stale || connection.state !== "connected" || session?.phase !== "ready") other++;
    else if (session.authorization?.trading_day === session.trading_day) authorized++;
    else ready++;
  }
  return t("CTP：{authorized} 个已授权 · {ready} 个只读 · {other} 个未就绪", {
    authorized,
    ready,
    other,
  });
}
export function Terminal() {
  return new URLSearchParams(location.search).get("screen") === "settings" ? (
    <TerminalWorkbench settingsWindow />
  ) : (
    <SetupGate>
      <TerminalWorkbench />
    </SetupGate>
  );
}
function TerminalWorkbench({ settingsWindow = false }: { settingsWindow?: boolean }) {
  const { locale } = useLocale();
  usePreferences();
  const [snapshot, setSnapshot] = useState<Snapshot | null>(null);
  const [drafts] = useState(createWorkspaceDrafts);
  const [headerTarget, setHeaderTarget] = useState<HTMLElement | null>(null);
  useEffect(() => setHeaderTarget(document.getElementById("terminal-header-tools")), []);
  const [view, setView] = useState(() => {
    try {
      const saved = localStorage.getItem("asterion.workspace");
      if (workspaces.some(workspace => workspace.id === saved)) return saved!;
    } catch {
      /* Storage may be unavailable; navigation still works. */
    }
    return "workspace.watchlist";
  });
  useEffect(() => {
    if (settingsWindow) return;
    try {
      localStorage.setItem("asterion.workspace", view);
    } catch {
      /* Keep the current window usable. */
    }
  }, [view, settingsWindow]);
  const [workspacePages, setWorkspacePages] = useState<Record<string, string>>({});
  const [workspaceParams, setWorkspaceParams] = useState<
    Record<string, Record<string, string> | undefined>
  >({});
  const [marketMode, setMarketMode] = useState<"live" | "history">("live");
  function showSettings(page?: SettingsPage) {
    void openSettings(page).catch(reason => setError(asDisplayError(reason)));
  }
  const [tasks, setTasks] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  const [checkedAt, setCheckedAt] = useState<number | null>(null);
  const generation = useRef(0);
  const revision = useRef<number | undefined>(undefined);
  const held = useRef<Snapshot | null>(null);
  // Every full snapshot carries the core's revision and the time the core last
  // refreshed it; stale probes keep the previous "checked at" time.
  const accept = useCallback((next: Snapshot) => {
    held.current = next;
    setSnapshot(next);
    setError("");
    revision.current = next.revision;
    if (!next.stale) setCheckedAt(next.refreshed_at_ms ?? Date.now());
  }, []);
  const refresh = useCallback(async () => {
    const current = ++generation.current;
    setBusy(true);
    try {
      const next = await request("runtime.snapshot");
      if (current === generation.current) accept(next);
    } catch (reason) {
      if (current === generation.current) setError(asDisplayError(reason));
    } finally {
      if (current === generation.current) setBusy(false);
    }
  }, [accept]);
  useEffect(() => {
    void refresh();
    return () => {
      // A counter, not a DOM ref: invalidates responses that arrive after unmount.
      // eslint-disable-next-line react-hooks/exhaustive-deps
      ++generation.current;
    };
  }, [refresh]);
  useEffect(() => {
    const key = (event: KeyboardEvent) => {
      if (!(event.metaKey || event.ctrlKey) || event.altKey || event.shiftKey || event.repeat)
        return;
      if (settingsWindow && event.key.toLowerCase() === "w") {
        event.preventDefault();
        void closeSettings().catch(reason => setError(asDisplayError(reason)));
        return;
      }
      if (event.key === ",") {
        event.preventDefault();
        if (!settingsWindow) showSettings();
      }
      const workspace = workspaces.find(
        item => item.id === workspaceShortcuts[Number(event.key) - 1],
      );
      if (workspace) {
        event.preventDefault();
        if (settingsWindow) return;
        setView(workspace.id);
      }
    };
    window.addEventListener("keydown", key);
    return () => window.removeEventListener("keydown", key);
  }, [settingsWindow]);
  const [visible, setVisible] = useState(() => !document.hidden);
  useEffect(() => {
    const update = () => setVisible(!document.hidden);
    document.addEventListener("visibilitychange", update);
    return () => document.removeEventListener("visibilitychange", update);
  }, []);
  const polling = !!snapshot && (Object.keys(snapshot.live).length > 0 || !!snapshot.nodes?.length);
  const live = !!snapshot?.market;
  useEffect(() => {
    // Polls read the core's published snapshot by revision; an unchanged
    // revision returns no state. Hidden windows stop polling.
    if (busy || !visible || !polling) return;
    let cancelled = false;
    const timer = window.setInterval(
      async () => {
        const current = generation.current;
        try {
          const next = await pollSnapshot(revision.current ?? 0, held.current);
          if (cancelled || current !== generation.current) return;
          if ("unchanged" in next) {
            setError("");
            setCheckedAt(next.refreshed_at_ms);
          } else accept(next);
        } catch (reason) {
          if (!cancelled && current === generation.current) setError(asDisplayError(reason));
        }
      },
      live ? 500 : 2000,
    );
    return () => {
      cancelled = true;
      window.clearInterval(timer);
    };
  }, [busy, visible, polling, live, accept]);
  async function trade(method: TerminalCommand, params: Record<string, unknown> = {}) {
    const current = ++generation.current;
    setBusy(true);
    try {
      const next = await request(method, params);
      if (current === generation.current) accept(next);
    } catch (reason) {
      // A durable commit can succeed even if the response is lost. Refresh state
      // before showing the error; never automatically repeat a trading command.
      try {
        const next = await request("runtime.snapshot");
        if (current === generation.current) {
          held.current = next;
          setSnapshot(next);
        }
      } catch {
        /* Keep the original command error. */
      }
      throw reason;
    } finally {
      if (current === generation.current) setBusy(false);
    }
  }
  const context: TerminalContext = {
    snapshot,
    busy,
    error,
    marketMode,
    workspacePage: workspacePages[view],
    workspaceParams: workspaceParams[view],
    query: (method, params) => request(method, params),
    navigate: (id, options) => {
      if (options?.marketMode) setMarketMode(options.marketMode);
      if (options?.page) setWorkspacePages(previous => ({ ...previous, [id]: options.page! }));
      setWorkspaceParams(previous => ({ ...previous, [id]: options?.params }));
      setView(id);
    },
    openSettings: showSettings,
    trade,
  };
  // Each plugin sees the shared context with trade scoped to its
  // declared commands.
  const currentPlugin = terminalPlugins.find(plugin => plugin.workspace.id === view)!;
  const current = currentPlugin.workspace;
  const Panel = current.component;
  const HeaderTools = current.headerTools;
  const WorkspaceToolbar = current.toolbar;
  const panelContext = scopedContext(currentPlugin, context);
  const backgroundTasks = terminalPlugins.flatMap(
    plugin => plugin.tasks?.(scopedContext(plugin, context)) ?? [],
  );
  const failedTasks =
    snapshot?.research?.tasks.filter(task => ["failed", "interrupted"].includes(task.state))
      .length ?? 0;
  return (
    <WindowFrame
      title={settingsWindow ? t("设置") + " — Asterion Terminal" : t("星枢 · Asterion Terminal")}
    >
      {!settingsWindow &&
        headerTarget &&
        HeaderTools &&
        createPortal(
          <WorkspaceDraftScope store={drafts} namespace={currentPlugin.id}>
            <HeaderTools {...panelContext} />
          </WorkspaceDraftScope>,
          headerTarget,
        )}
      {settingsWindow ? (
        <Settings
          initialPage={settingsPage(new URLSearchParams(location.search).get("category"))}
          trade={trade}
          plugins={terminalPlugins}
          snapshot={snapshot}
          refresh={() => void refresh()}
          error={error}
          busy={busy}
        />
      ) : (
        <Workbench
          title={current.title}
          showToolbar={!current.hideToolbar && !!WorkspaceToolbar}
          navigation={workspaces.map(item => ({
            ...item,
            active: item.id === view,
            shortcut: `⌘${workspaceShortcuts.indexOf(item.id) + 1} / Ctrl+${workspaceShortcuts.indexOf(item.id) + 1}`,
            select: () => context.navigate(item.id),
          }))}
          rail={
            <>
              <button
                aria-label={t("任务中心")}
                title={t("任务中心")}
                aria-pressed={tasks}
                onClick={() => setTasks(!tasks)}
              >
                <Icon name="tasks" />
              </button>
              <button
                aria-label={t("设置")}
                title={`${t("设置")} · ⌘, / Ctrl+,`}
                onClick={() => showSettings()}
              >
                <Icon name="settings" />
              </button>
            </>
          }
          toolbar={WorkspaceToolbar && <WorkspaceToolbar {...panelContext} />}
          notices={
            error ? (
              <div className="alert" role="alert">
                <ErrorNotice error={error} namespace="host" />
                <span className="panel-spacer" />
                <button disabled={busy} onClick={() => void refresh()}>
                  {t("重试连接")}
                </button>
              </div>
            ) : !snapshot ? (
              <div className="notice" role="status">
                {t("正在启动本机 C++ 核心…")}
              </div>
            ) : null
          }
          body={
            <main className="terminal-business">
              <Suspense fallback={<p role="status">{t("正在加载工作区…")}</p>}>
                <WorkspaceDraftScope store={drafts} namespace={currentPlugin.id}>
                  <Panel {...panelContext} />
                </WorkspaceDraftScope>
              </Suspense>
            </main>
          }
          dock={
            tasks && (
              <section className="task-dock terminal-tasks" aria-label={t("任务中心")}>
                <div className="panel-heading">
                  <h2>{t("任务中心")}</h2>
                  <span className="panel-spacer" />
                  <button onClick={() => setTasks(false)} aria-label={t("关闭任务中心")}>
                    <Icon name="close" size={14} />
                  </button>
                </div>
                {backgroundTasks.length ? (
                  <div className="terminal-task-list">
                    {backgroundTasks.map(task => (
                      <button
                        key={task.id}
                        onClick={() => {
                          setTasks(false);
                          task.open();
                        }}
                      >
                        <strong>{task.title}</strong>
                        <span>
                          {task.status} · {task.completed} / {task.total}
                        </span>
                      </button>
                    ))}
                  </div>
                ) : (
                  <p className="content-caption">
                    {busy ? t("正在处理本机请求…") : t("暂无后台任务")}
                  </p>
                )}
              </section>
            )
          }
          status={
            <>
              <ServiceStatus
                snapshot={snapshot}
                failed={!!error}
                busy={busy}
                checkedAt={checkedAt}
                refresh={() => void refresh()}
                settings={() => showSettings("connections")}
              />
              <span className="status-divider" />
              <button
                className={failedTasks ? "bad" : undefined}
                aria-expanded={tasks}
                onClick={() => setTasks(!tasks)}
              >
                {t("{p0} 项任务执行中", {
                  p0:
                    snapshot?.research?.tasks.filter(task =>
                      ["running", "queued", "cancel_requested"].includes(task.state),
                    ).length ?? 0,
                })}
                {failedTasks > 0 && ` · ${t("{p0} 项任务异常", { p0: failedTasks })}`}
              </button>
              <span className="panel-spacer" />
              <span className="status-updated">
                {error || snapshot?.stale
                  ? t("状态待确认")
                  : checkedAt
                    ? t("状态读取于 {p0}", { p0: new Date(checkedAt).toLocaleTimeString(locale) })
                    : t("正在读取状态…")}
              </span>
              <span className="status-divider" />
              <button onClick={() => context.navigate("workspace.trading")}>
                {liveSummary(snapshot)}
              </button>
            </>
          }
        />
      )}
    </WindowFrame>
  );
}
