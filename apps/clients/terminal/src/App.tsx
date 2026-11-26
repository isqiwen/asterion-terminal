import type { CommandCall } from "./bridge/commands";
import { Icon } from "./ui/Icon";
import { openSettings, closeSettings, settingsPage, type SettingsPage } from "./settings/window";
import { LabHeaderActivity } from "./host/components/LabActivityPopover";
import { StatusIndicators } from "./host/components/StatusIndicators";
import { LabActivityProvider } from "./host/lab-activity";
import { NotificationCenter, notify, useErrorNotification } from "./host/notifications";
import { BackendError, asDisplayError, type DisplayError } from "./i18n/errors";
import { translate, useLocale, type MessageValues } from "./i18n";
const t = (key: string, values?: MessageValues) => translate("host", key, values);
import { SetupGate } from "./startup/SetupGate";
import {
  OpenLiveTrading,
  TradingAccountsProvider,
  useTradingAccounts,
} from "./host/trading-accounts";
import { MarketProvider } from "./host/market";
import { WarehouseProvider, useWarehouse } from "./host/warehouse";
import {
  Component,
  Suspense,
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState,
  type ReactNode,
} from "react";
import { WindowFrame } from "@asterion/workbench/components/WindowFrame";
import { Workbench } from "@asterion/workbench/workspace/Workbench";
import { usePreferences } from "./ui/preferences";
import { request, pollSnapshot, type Snapshot } from "@asterion/desktop-bridge/client";
import { Settings } from "./settings/Settings";
import { terminalFeatures, workspaces } from "./features";
import { WorkspaceActiveContext } from "./host/polling";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "./host/workspace/drafts";
import {
  modules,
  initialNavigation,
  currentRoute,
  navigationLocation,
  selectModule,
  selectView,
} from "./host/navigation";
import type { TerminalContext } from "./host/feature-registry";
import "./ui/theme/style.css";
import "./terminal.css";
import "./host/workspace/workbench.css";
import "./settings/settings.css";
import "./ui/workflows.css";
class TerminalErrorBoundary extends Component<{ children: ReactNode }, { error: Error | null }> {
  state: { error: Error | null } = { error: null };

  static getDerivedStateFromError(error: Error) {
    return { error };
  }

  render() {
    if (!this.state.error) return this.props.children;
    return (
      <div className="workspace-empty" role="alert">
        <p>{t("界面发生意外错误")}</p>
        <p className="subtle">{this.state.error.message}</p>
        <div className="workspace-empty-actions">
          <button type="button" className="primary" onClick={() => window.location.reload()}>
            {t("重新加载")}
          </button>
        </div>
      </div>
    );
  }
}

// Subtabs only appear when a module's remote service has data to switch between.
export function ModuleHeader({
  module,
  view,
  warehouseConfigured,
  tradingConnections,
  navigate,
}: {
  module: (typeof modules)[number];
  view: string;
  warehouseConfigured: boolean;
  tradingConnections: readonly { id: string }[];
  navigate: TerminalContext["navigate"];
}) {
  const viewsRef = useRef<HTMLElement>(null);
  const { views: tradingViews } = useTradingAccounts();
  const warehouse = useWarehouse();
  const visible =
    module.views.length > 1 &&
    (module.id === "data"
      ? warehouseConfigured && warehouse?.error === ""
      : module.id === "live"
        ? tradingConnections.some(item => {
            const state = tradingViews[item.id];
            return !!(state?.emergency || state?.account || state?.strategies);
          })
        : true);
  useEffect(() => {
    if (!visible) return;
    viewsRef.current?.querySelector('[aria-current="page"]')?.scrollIntoView({
      block: "nearest",
      inline: "nearest",
    });
  }, [visible, module.id, view]);
  if (!visible) return null;
  return (
    <div className="module-header">
      <nav ref={viewsRef} className="module-views" aria-label={t("子模块视图")}>
        {module.views.map(id => (
          <button
            key={id}
            type="button"
            aria-current={view === id ? "page" : undefined}
            onClick={() => navigate(id)}
          >
            {workspaces.find(item => item.id === id)!.title}
          </button>
        ))}
      </nav>
      {module.id === "research" && (
        <LabHeaderActivity
          openRun={id => navigate("workspace.lab-runs", { params: { run: id } })}
          openSweep={id => navigate("workspace.lab-runs", { params: { sweep: id } })}
          openFactors={() => navigate("workspace.lab-factors")}
        />
      )}
    </div>
  );
}

export function Terminal() {
  return (
    <TerminalErrorBoundary>
      {new URLSearchParams(location.search).get("screen") === "settings" ? (
        <TerminalWorkbench settingsWindow />
      ) : (
        <SetupGate>
          <TerminalWorkbench />
        </SetupGate>
      )}
    </TerminalErrorBoundary>
  );
}
function TerminalWorkbench({ settingsWindow = false }: { settingsWindow?: boolean }) {
  useLocale();
  usePreferences();
  const [snapshot, setSnapshot] = useState<Snapshot | null>(null);
  const [drafts] = useState(createWorkspaceDrafts);
  const [userNavigated, setUserNavigated] = useState(false);
  const [navigation, setNavigation] = useState(() => {
    try {
      return initialNavigation(JSON.parse(localStorage.getItem("asterion.navigation") ?? "null"));
    } catch {
      return initialNavigation();
    }
  });
  const route = currentRoute(navigation);
  const view = route.view;
  const [visitedViews, setVisitedViews] = useState<string[]>(() => [view]);
  useEffect(() => {
    setVisitedViews(previous => (previous.includes(view) ? previous : [...previous, view]));
  }, [view]);
  const mountedViews = visitedViews.includes(view) ? visitedViews : [...visitedViews, view];
  // Lab leaving the ready state, lost log lines and a failed background
  // refresh are reported when they begin.
  const reported = useRef({ lab: false, logs: 0, refresh: false });
  useErrorNotification(
    settingsWindow
      ? undefined
      : snapshot?.lab?.error
        ? new BackendError("operation_failed", snapshot.lab.error)
        : snapshot?.lab?.state === "ready"
          ? ""
          : undefined,
    "asterion.terminal.research",
  );
  useEffect(() => {
    // The main window reports them; the settings window reads the same snapshot.
    if (settingsWindow || !snapshot) return;
    const before = reported.current;
    const lab = snapshot.lab?.state === "ready";
    if (before.lab && !lab && !snapshot.lab?.error)
      notify(new BackendError("service_restarted", ""), {
        severity: "warning",
        namespace: "asterion.terminal.research",
      });
    const { log_failures: logs, refresh_failed: refresh } = snapshot.diagnostics;
    if (logs > before.logs)
      notify("本次运行有日志写入失败，诊断记录可能不完整。请检查日志目录的空间和权限。", {
        severity: "warning",
      });
    if (refresh && !before.refresh) notify("后台状态刷新失败，当前显示为最近成功读取的状态。");
    reported.current = { lab, logs, refresh: !!refresh };
  }, [snapshot, settingsWindow]);
  useEffect(() => {
    if (settingsWindow) return;
    try {
      localStorage.setItem("asterion.navigation", JSON.stringify(navigationLocation(navigation)));
    } catch {
      /* Navigation remains available without UI storage. */
    }
  }, [navigation, settingsWindow]);
  const showSettings = useCallback((page?: SettingsPage) => {
    void openSettings(page).catch(reason => setError(asDisplayError(reason)));
  }, []);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  // The main window polls the same snapshot and reports core connection errors.
  useErrorNotification(settingsWindow ? undefined : error);
  const generation = useRef(0);
  // Commands in flight. A cancel, a strategy stop or a revoke may run beside
  // another command.
  const commands = useRef(0);
  const refreshing = useRef(false);
  const syncBusy = useCallback(() => {
    setBusy(refreshing.current || commands.current > 0);
  }, []);
  const revision = useRef<number | undefined>(undefined);
  // Every full snapshot carries the core's revision and the time the core last
  // refreshed it.
  const accept = useCallback((next: Snapshot) => {
    if (next.revision < (revision.current ?? 0)) return;
    setSnapshot(next);
    setError("");
    revision.current = next.revision;
  }, []);
  useEffect(() => {
    const channel = new BroadcastChannel("asterion.snapshot");
    channel.onmessage = ({ data }: MessageEvent<Snapshot>) => accept(data);
    return () => channel.close();
  }, [accept]);
  const refresh = useCallback(async () => {
    const current = ++generation.current;
    refreshing.current = true;
    syncBusy();
    try {
      const next = await request("runtime.snapshot");
      if (current === generation.current) accept(next);
    } catch (reason) {
      if (current === generation.current) setError(asDisplayError(reason));
    } finally {
      if (current === generation.current) {
        refreshing.current = false;
        syncBusy();
      }
    }
  }, [accept, syncBusy]);
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
      if (event.target instanceof HTMLElement && event.target.isContentEditable) return;
      if (settingsWindow && event.key.toLowerCase() === "w") {
        event.preventDefault();
        void closeSettings().catch(reason => setError(asDisplayError(reason)));
        return;
      }
      if (event.key === ",") {
        event.preventDefault();
        if (!settingsWindow) showSettings();
      }
      const module = modules[Number(event.key) - 1];
      if (module) {
        event.preventDefault();
        if (!settingsWindow) {
          setUserNavigated(true);
          setNavigation(previous => selectModule(previous, module.id));
        }
      }
    };
    window.addEventListener("keydown", key);
    return () => window.removeEventListener("keydown", key);
  }, [settingsWindow, showSettings]);
  const polling = !!snapshot;
  const pollInFlight = useRef(false);
  useEffect(() => {
    // Polls read the core's published snapshot by revision; an unchanged
    // revision returns no state. A command in flight does not stop polling.
    if (!polling) return;
    let cancelled = false;
    const timer = window.setInterval(async () => {
      // Keep one read per window.
      if (pollInFlight.current) return;
      pollInFlight.current = true;
      const current = generation.current;
      try {
        const next = await pollSnapshot(revision.current ?? 0);
        if (cancelled || current !== generation.current || next.revision < (revision.current ?? 0))
          return;
        if ("unchanged" in next) setError("");
        else accept(next);
      } catch (reason) {
        if (!cancelled && current === generation.current) setError(asDisplayError(reason));
      } finally {
        pollInFlight.current = false;
      }
    }, 2000);
    return () => {
      cancelled = true;
      window.clearInterval(timer);
    };
  }, [polling, accept]);
  const trade = useCallback<TerminalContext["trade"]>(
    async (...args: CommandCall) => {
      const current = ++generation.current;
      refreshing.current = false;
      commands.current += 1;
      syncBusy();
      try {
        const next = await request(...args);
        if (current === generation.current) accept(next);
        const channel = new BroadcastChannel("asterion.snapshot");
        channel.postMessage(next);
        channel.close();
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
        syncBusy();
      }
    },
    [accept, syncBusy],
  );
  const navigate = useCallback<TerminalContext["navigate"]>((id, options) => {
    setUserNavigated(true);
    setNavigation(previous => selectView(previous, id, options));
  }, []);
  const baseContext = useMemo<TerminalContext>(
    () => ({
      snapshot,
      busy,
      error,
      navigate,
      openSettings: showSettings,
      trade,
    }),
    [snapshot, busy, error, navigate, showSettings, trade],
  );
  const context = useMemo<TerminalContext>(
    () => ({
      ...baseContext,
      workspacePage: route.options.page,
      workspaceParams: route.options.params,
    }),
    [baseContext, route.options.page, route.options.params],
  );
  // The selected built-in feature uses the application context.
  const currentFeature = terminalFeatures.find(feature =>
    feature.workspaces.some(item => item.id === view),
  );
  const current = currentFeature?.workspaces.find(item => item.id === view);
  const WorkspaceToolbar = current?.toolbar;
  const marketEndpoint = snapshot?.market_endpoint;
  const warehouseEndpoint = snapshot?.warehouse_endpoint;
  const panelContext = context;
  const tradingConnections = useMemo(
    () => (snapshot?.trading_connections ?? []).flatMap(item => ("error" in item ? [] : [item])),
    [snapshot?.trading_connections],
  );
  return (
    <WindowFrame
      title={settingsWindow ? t("设置") + " — Asterion Terminal" : t("星枢 · Asterion Terminal")}
    >
      {settingsWindow ? (
        <Settings
          initialPage={settingsPage(new URLSearchParams(location.search).get("category"))}
          trade={trade}
          snapshot={snapshot}
          busy={busy}
        />
      ) : (
        <TradingAccountsProvider connections={tradingConnections}>
          <OpenLiveTrading
            accounts={tradingConnections.length}
            cancelled={userNavigated}
            open={() => setNavigation(previous => selectModule(previous, "live"))}
          />
          <MarketProvider
            endpoint={
              marketEndpoint && "host" in marketEndpoint
                ? `${marketEndpoint.host}:${marketEndpoint.port}/${marketEndpoint.fingerprint}`
                : ""
            }
          >
            <WarehouseProvider
              endpoint={
                warehouseEndpoint && "host" in warehouseEndpoint
                  ? `${warehouseEndpoint.host}:${warehouseEndpoint.port}/${warehouseEndpoint.fingerprint}`
                  : ""
              }
            >
              <LabActivityProvider
                ready={snapshot?.lab?.state === "ready"}
                currentModule={navigation.module}
                openRun={id => context.navigate("workspace.lab-runs", { params: { run: id } })}
                openSweep={id => context.navigate("workspace.lab-runs", { params: { sweep: id } })}
              >
                <Workbench
                  showToolbar={!current?.hideToolbar && !!WorkspaceToolbar}
                  navigation={modules.map((item, index) => ({
                    id: item.id,
                    title: t(item.title),
                    icon: <Icon name={item.icon} />,
                    active: item.id === navigation.module,
                    shortcut: `⌘${index + 1} / Ctrl+${index + 1}`,
                    select: () => {
                      setUserNavigated(true);
                      setNavigation(previous => selectModule(previous, item.id));
                    },
                  }))}
                  rail={
                    <button
                      type="button"
                      aria-label={t("设置")}
                      title={`${t("设置")} · ⌘, / Ctrl+,`}
                      onClick={() => showSettings()}
                    >
                      <Icon name="settings" />
                    </button>
                  }
                  toolbar={WorkspaceToolbar && <WorkspaceToolbar {...panelContext} />}
                  notices={
                    !snapshot && !error ? (
                      <div className="notice" role="status">
                        {t("正在启动本机 C++ 核心…")}
                      </div>
                    ) : null
                  }
                  body={
                    <div className="module-workspace">
                      <ModuleHeader
                        module={route.module}
                        view={view}
                        warehouseConfigured={!!(warehouseEndpoint && "host" in warehouseEndpoint)}
                        tradingConnections={tradingConnections}
                        navigate={context.navigate}
                      />
                      <main className="terminal-business">
                        <h1 className="sr-only">{t(route.module.title)}</h1>
                        {mountedViews.map(id => {
                          const feature = terminalFeatures.find(item =>
                            item.workspaces.some(workspace => workspace.id === id),
                          );
                          const workspace = feature?.workspaces.find(item => item.id === id);
                          if (!feature || !workspace) return null;
                          const ViewPanel = workspace.component;
                          const isActive = id === view;
                          return (
                            <div key={id} className="workspace-panel" hidden={!isActive}>
                              <WorkspaceActiveContext.Provider value={isActive}>
                                <WorkspaceDraftScope store={drafts} namespace={feature.id}>
                                  <Suspense fallback={null}>
                                    <ViewPanel {...(isActive ? panelContext : baseContext)} />
                                  </Suspense>
                                </WorkspaceDraftScope>
                              </WorkspaceActiveContext.Provider>
                            </div>
                          );
                        })}
                      </main>
                    </div>
                  }

                  status={
                    <>
                      <StatusIndicators
                        snapshot={snapshot}
                        openMarketSettings={() => showSettings("market")}
                        openMarket={() => context.navigate("workspace.market")}
                        openResearch={() => context.navigate("workspace.lab-strategies")}
                        openRun={id =>
                          context.navigate("workspace.lab-runs", { params: { run: id } })
                        }
                        openSweep={id =>
                          context.navigate("workspace.lab-runs", { params: { sweep: id } })
                        }
                        openFactors={() => context.navigate("workspace.lab-factors")}
                        openWarehouseSettings={() => showSettings("warehouse")}
                        openDataset={() => context.navigate("workspace.data-dataset")}
                        openTradingSettings={() => showSettings("trading")}
                        openTrading={account =>
                          context.navigate(
                            "workspace.trading",
                            account ? { params: { account } } : undefined,
                          )
                        }
                      />
                      <div className="status-actions">
                        <NotificationCenter />
                        {error && (
                          <button type="button" disabled={busy} onClick={() => void refresh()}>
                            {t("重试连接")}
                          </button>
                        )}
                      </div>
                    </>
                  }
                />
              </LabActivityProvider>
            </WarehouseProvider>
          </MarketProvider>
        </TradingAccountsProvider>
      )}
    </WindowFrame>
  );
}
