import { openSettings, closeSettings, settingsPage } from "./settings/window";
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
  type CsvRequest,
  type Snapshot,
  type TerminalCommand,
} from "@asterion/desktop-bridge/client";
import { Settings } from "./settings/Settings";
import { terminalPlugins, workspaces } from "./plugins";
import type { TerminalContext } from "../plugins/contract";
import "./ui/theme/style.css";
import "./terminal.css";
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
  const [view, setView] = useState("workspace.overview");
  const [marketMode, setMarketMode] = useState<"live" | "history">("live");
  function showSettings(page: "appearance" | "connections" | "general" = "general") {
    void openSettings(page).catch(reason => setError(asDisplayError(reason)));
  }
  const [tasks, setTasks] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  const [checkedAt, setCheckedAt] = useState<number | null>(null);
  const generation = useRef(0);
  const refresh = useCallback(async () => {
    const current = ++generation.current;
    setBusy(true);
    try {
      const next = await request("runtime.snapshot");
      if (current === generation.current) {
        setSnapshot(next);
        setError("");
        if (!next.stale) setCheckedAt(Date.now());
      }
    } catch (reason) {
      if (current === generation.current) setError(asDisplayError(reason));
    } finally {
      if (current === generation.current) setBusy(false);
    }
  }, []);
  useEffect(() => {
    void refresh();
    return () => {
      ++generation.current;
    };
  }, [refresh]);
  useEffect(() => {
    const key = (event: KeyboardEvent) => {
      if (!(event.metaKey || event.ctrlKey) || event.altKey || event.shiftKey || event.repeat)
        return;
      if (event.key === ",") {
        event.preventDefault();
        showSettings();
      }
      const workspace = workspaces[Number(event.key) - 1];
      if (workspace) {
        event.preventDefault();
        if (settingsWindow) return;
        setView(workspace.id);
      }
    };
    window.addEventListener("keydown", key);
    return () => window.removeEventListener("keydown", key);
  }, []);
  const [visible, setVisible] = useState(() => !document.hidden);
  useEffect(() => {
    const update = () => setVisible(!document.hidden);
    document.addEventListener("visibilitychange", update);
    return () => document.removeEventListener("visibilitychange", update);
  }, []);
  useEffect(() => {
    // Hidden windows stop polling; becoming visible re-runs this effect immediately.
    if (busy || !visible || !snapshot || (!snapshot.connection && !snapshot.nodes?.length)) return;
    let cancelled = false;
    const timer = window.setTimeout(
      async () => {
        const current = generation.current;
        try {
          const next = await request("runtime.snapshot");
          if (!cancelled && current === generation.current) {
            setSnapshot(next);
            setError("");
            if (!next.stale) setCheckedAt(Date.now());
          }
        } catch (reason) {
          if (!cancelled && current === generation.current) setError(asDisplayError(reason));
        }
      },
      snapshot.market ? 500 : 2000,
    );
    return () => {
      cancelled = true;
      window.clearTimeout(timer);
    };
  }, [busy, snapshot, visible]);
  async function inspect(params: CsvRequest) {
    const current = ++generation.current;
    setBusy(true);
    try {
      const next = await request("futures.inspect_csv", params);
      if (current === generation.current) {
        setSnapshot(next);
        setError("");
        setCheckedAt(Date.now());
      }
    } finally {
      if (current === generation.current) setBusy(false);
    }
  }
  async function trade(method: TerminalCommand, params: Record<string, unknown> = {}) {
    const current = ++generation.current;
    setBusy(true);
    try {
      const next = await request(method, params);
      if (current === generation.current) {
        setSnapshot(next);
        setError("");
        setCheckedAt(Date.now());
      }
    } catch (reason) {
      // A durable commit can succeed even if the response is lost. Refresh state
      // before showing the error; never automatically repeat a trading command.
      try {
        const next = await request("runtime.snapshot");
        if (current === generation.current) setSnapshot(next);
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
    widgets: [],
    marketMode,
    navigate: (id, options) => {
      if (options) setMarketMode(options.marketMode);
      setView(id);
    },
    openSettings: showSettings,
    openTasks: () => setTasks(true),
    refresh: () => void refresh(),
    inspect,
    trade,
  };
  context.widgets = terminalPlugins.flatMap(plugin => plugin.widgets?.(context) ?? []);
  const current = workspaces.find(item => item.id === view)!;
  const Panel = current.component;
  const backgroundTasks = terminalPlugins.flatMap(plugin => plugin.tasks?.(context) ?? []);
  return (
    <WindowFrame
      title={settingsWindow ? t("设置") + " — Asterion Terminal" : t("星枢 · Asterion Terminal")}
      language={locale === "zh-CN" ? "zh" : "en"}
    >
      {settingsWindow ? (
        <Settings
          initialPage={settingsPage(new URLSearchParams(location.search).get("category"))}
          trade={trade}
          plugins={terminalPlugins}
          snapshot={snapshot}
          close={() => {
            void closeSettings().catch(reason => setError(asDisplayError(reason)));
          }}
          refresh={() => void refresh()}
          error={error}
          busy={busy}
        />
      ) : (
        <Workbench
          title={current.title}
          navigation={workspaces.map((item, index) => ({
            ...item,
            active: item.id === view,
            shortcut: `⌘${index + 1}`,
            select: () => setView(item.id),
          }))}
          sections={
            view === "workspace.overview"
              ? []
              : [
                  {
                    id: `${current.id}.main`,
                    title: current.section,
                    active: true,
                    select: () => {},
                  },
                ]
          }
          rail={
            <>
              <button
                aria-label={t("任务中心")}
                title={t("任务中心")}
                aria-pressed={tasks}
                onClick={() => setTasks(!tasks)}
              >
                ☷
              </button>
              <button aria-label={t("设置")} title={t("设置 · ⌘,")} onClick={() => showSettings()}>
                ⚙
              </button>
            </>
          }
          toolbar={
            <>
              <span className="panel-spacer" />
              <span className="subtle">{t("期货")}</span>
              <button className={tasks ? "pressed" : ""} onClick={() => setTasks(!tasks)}>
                {t("任务")}
              </button>
            </>
          }
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
                <Panel {...context} />
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
                    ×
                  </button>
                </div>
                {backgroundTasks.length ? (
                  <div className="terminal-task-list">
                    {backgroundTasks.map(task => (
                      <button key={task.id} onClick={task.open}>
                        <strong>{task.title}</strong>
                        <span>
                          {task.status} · {task.completed} / {task.total}
                        </span>
                      </button>
                    ))}
                  </div>
                ) : (
                  <p className="dashboard-caption">
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
              <button aria-expanded={tasks} onClick={() => setTasks(!tasks)}>
                {t("{p0} 项任务执行中", {
                  p0:
                    (snapshot?.research?.tasks.filter(task =>
                      ["running", "queued", "cancel_requested"].includes(task.state),
                    ).length ?? 0) + (snapshot?.strategy?.phase === "running" ? 1 : 0),
                })}
              </button>
              <span className="panel-spacer" />
              <span className="status-updated">
                {error
                  ? t("状态待确认")
                  : checkedAt
                    ? t("状态读取于 {p0}", { p0: new Date(checkedAt).toLocaleTimeString(locale) })
                    : t("正在读取状态…")}
              </span>
              <span className="status-divider" />
              <button onClick={() => setView("workspace.trading")}>
                {snapshot?.paper ? t("历史模拟交易") : t("交易未连接")}
              </button>
            </>
          }
        />
      )}
    </WindowFrame>
  );
}
