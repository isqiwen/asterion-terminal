import type { CommandCall } from "./bridge/commands";
import { TaskPagination } from "./ui/TaskPagination";
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
import { request, pollSnapshot, type Snapshot } from "@asterion/desktop-bridge/client";
import { Settings } from "./settings/Settings";
import { terminalPlugins, workspaces } from "./plugins";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "./host/workspace/drafts";
import {
  modules,
  initialNavigation,
  currentRoute,
  navigationLocation,
  selectModule,
  selectSection,
  selectView,
} from "./host/navigation";
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
    if (connection.state !== "connected" || session?.phase !== "ready") other++;
    else if (session.authorization) authorized++;
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
  const [navigation, setNavigation] = useState(() => {
    try {
      return initialNavigation(JSON.parse(localStorage.getItem("asterion.navigation") ?? "null"));
    } catch {
      return initialNavigation();
    }
  });
  const route = currentRoute(navigation);
  const view = route.view;
  const sectionsRef = useRef<HTMLElement>(null);
  useEffect(() => {
    sectionsRef.current?.querySelector('[aria-current="page"]')?.scrollIntoView({
      block: "nearest",
      inline: "nearest",
    });
  }, [navigation.module, route.section.id]);
  useEffect(() => {
    if (settingsWindow) return;
    try {
      localStorage.setItem("asterion.navigation", JSON.stringify(navigationLocation(navigation)));
    } catch {
      /* Navigation remains available without UI storage. */
    }
  }, [navigation, settingsWindow]);
  function showSettings(page?: SettingsPage) {
    void openSettings(page).catch(reason => setError(asDisplayError(reason)));
  }
  const [tasks, setTasks] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  const [checkedAt, setCheckedAt] = useState<number | null>(null);
  const generation = useRef(0);
  // Commands in flight. A cancel, a strategy stop or a revoke may run beside
  // another command.
  const commands = useRef(0);
  const revision = useRef<number | undefined>(undefined);
  const held = useRef<Snapshot | null>(null);
  // Every full snapshot carries the core's revision and the time the core last
  // refreshed it.
  const accept = useCallback((next: Snapshot) => {
    if (next.revision < (revision.current ?? 0)) return;
    held.current = next;
    setSnapshot(next);
    setError("");
    revision.current = next.revision;
    setCheckedAt(next.refreshed_at_ms ?? Date.now());
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
      const module = modules.slice(0, 7)[Number(event.key) - 1];
      if (module) {
        event.preventDefault();
        if (!settingsWindow) setNavigation(previous => selectModule(previous, module.id));
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
  const pollInFlight = useRef(false);
  useEffect(() => {
    // Polls read the core's published snapshot by revision; an unchanged
    // revision returns no state. Hidden windows stop polling. A command in
    // flight does not: accounts and quotes are still observed while it waits.
    if (!visible || !polling) return;
    let cancelled = false;
    const timer = window.setInterval(
      async () => {
        // Keep one read per window, including across visibility changes.
        if (pollInFlight.current) return;
        pollInFlight.current = true;
        const current = generation.current;
        try {
          const next = await pollSnapshot(revision.current ?? 0, held.current);
          if (
            cancelled ||
            current !== generation.current ||
            next.revision < (revision.current ?? 0)
          )
            return;
          if ("unchanged" in next) {
            setError("");
            setCheckedAt(next.refreshed_at_ms);
          } else accept(next);
        } catch (reason) {
          if (!cancelled && current === generation.current) setError(asDisplayError(reason));
        } finally {
          pollInFlight.current = false;
        }
      },
      live ? 500 : 2000,
    );
    return () => {
      cancelled = true;
      window.clearInterval(timer);
    };
  }, [visible, polling, live, accept]);
  async function trade(...args: CommandCall) {
    const current = ++generation.current;
    commands.current += 1;
    setBusy(true);
    try {
      const next = await request(...args);
      if (current === generation.current) accept(next);
    } catch (reason) {
      // A durable commit can succeed even if the response is lost. Refresh state
      // before showing the error; never automatically repeat a trading command.
      try {
        const next = await request("runtime.snapshot");
        if (current === generation.current) accept(next);
      } catch {
        /* Keep the original command error. */
      }
      throw reason;
    } finally {
      commands.current -= 1;
      if (!commands.current) setBusy(false);
    }
  }
  const context: TerminalContext = {
    snapshot,
    busy,
    error,
    marketMode: route.options.marketMode ?? "live",
    workspacePage: route.options.page,
    workspaceParams: route.options.params,
    query: (...args) => request(...args),
    navigate: (id, options) => setNavigation(previous => selectView(previous, id, options)),
    openSettings: showSettings,
    trade,
  };
  // Each plugin sees the shared context with trade scoped to its
  // declared commands.
  const currentPlugin = terminalPlugins.find(plugin =>
    plugin.workspaces.some(item => item.id === view),
  );
  const current = currentPlugin?.workspaces.find(item => item.id === view);
  const Panel = current?.component;
  const HeaderTools = current?.headerTools;
  const WorkspaceToolbar = current?.toolbar;
  const panelContext = currentPlugin ? scopedContext(currentPlugin, context) : context;
  const backgroundTasks = terminalPlugins.flatMap(
    plugin => plugin.tasks?.(scopedContext(plugin, context)) ?? [],
  );
  const failedTasks =
    (snapshot?.task_service?.failed_count ?? 0) + (snapshot?.task_service?.interrupted_count ?? 0);
  return (
    <WindowFrame
      title={settingsWindow ? t("设置") + " — Asterion Terminal" : t("星枢 · Asterion Terminal")}
    >
      {!settingsWindow &&
        headerTarget &&
        HeaderTools &&
        createPortal(
          <WorkspaceDraftScope store={drafts} namespace={current!.id}>
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
          title={t(route.module.title)}
          showToolbar={!current?.hideToolbar && !!WorkspaceToolbar}
          navigation={modules
            .filter(item => item.id !== "system")
            .map((item, index) => ({
              id: item.id,
              title: t(item.title),
              icon: <Icon name={item.icon} />,
              active: item.id === navigation.module,
              shortcut: `⌘${index + 1} / Ctrl+${index + 1}`,
              select: () => setNavigation(previous => selectModule(previous, item.id)),
            }))}
          rail={
            <>
              <button
                aria-label={t("任务中心")}
                title={t("任务中心")}
                aria-expanded={tasks}
                onClick={() => setTasks(!tasks)}
              >
                <Icon name="tasks" />
              </button>
              <button
                aria-label={t("系统管理")}
                title={t("系统管理")}
                aria-current={navigation.module === "system" ? "page" : undefined}
                onClick={() => setNavigation(previous => selectModule(previous, "system"))}
              >
                <Icon name="connections" />
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
            <div className="module-workspace">
              <nav ref={sectionsRef} className="module-sections" aria-label={t("业务子模块")}>
                {route.module.sections.map(section => (
                  <button
                    key={section.id}
                    aria-current={section.id === route.section.id ? "page" : undefined}
                    onClick={() => setNavigation(previous => selectSection(previous, section.id))}
                  >
                    {t(section.title)}
                  </button>
                ))}
              </nav>
              {route.section.views.length > 1 && (
                <nav className="module-views" aria-label={t("子模块视图")}>
                  {route.section.views.map(id => (
                    <button
                      key={id}
                      aria-current={view === id ? "page" : undefined}
                      onClick={() => context.navigate(id)}
                    >
                      {workspaces.find(item => item.id === id)!.title}
                    </button>
                  ))}
                </nav>
              )}
              <main className="terminal-business">
                <Suspense fallback={<p role="status">{t("正在加载工作区…")}</p>}>
                  {Panel && currentPlugin ? (
                    <WorkspaceDraftScope store={drafts} namespace={current!.id}>
                      <Panel {...panelContext} />
                    </WorkspaceDraftScope>
                  ) : (
                    <section className="module-empty" aria-label={t(route.section.title)}>
                      <h2>{t(route.section.title)}</h2>
                      {navigation.module === "system" ? (
                        <>
                          <p>{t("管理本机配置，查看任务与服务状态。")}</p>
                          {route.section.id === "setup" && (
                            <>
                              <button onClick={() => showSettings("preferences")}>
                                {t("偏好设置")}
                              </button>
                              <button onClick={() => showSettings("sources")}>{t("数据源")}</button>
                              <button onClick={() => showSettings("ctp")}>{t("CTP 账户")}</button>
                              <button onClick={() => showSettings("plugins")}>{t("插件")}</button>
                            </>
                          )}
                          {route.section.id === "tasks" && (
                            <>
                              <button onClick={() => setTasks(true)}>{t("任务中心")}</button>
                              <button onClick={() => showSettings("connections")}>
                                {t("连接与部署")}
                              </button>
                            </>
                          )}
                          {route.section.id === "records" && (
                            <p>{t("资料备份、恢复和诊断导出尚未实现。")}</p>
                          )}
                        </>
                      ) : (
                        <>
                          <p role="status">{t("此子模块的完整功能尚未实现。")}</p>
                          {route.module.sections
                            .flatMap(section => section.views)
                            .map(id => (
                              <button key={id} onClick={() => context.navigate(id)}>
                                {workspaces.find(item => item.id === id)!.title}
                              </button>
                            ))}
                        </>
                      )}
                    </section>
                  )}
                </Suspense>
              </main>
            </div>
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
                <TaskPagination
                  taskService={snapshot?.task_service}
                  busy={busy}
                  trade={context.trade}
                />
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
                    snapshot?.task_service?.tasks.filter(task =>
                      ["running", "queued", "cancel_requested", "publishing"].includes(task.state),
                    ).length ?? 0,
                })}
                {failedTasks > 0 && ` · ${t("{p0} 项任务异常", { p0: failedTasks })}`}
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
