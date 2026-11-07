import type { CommandCall } from "./bridge/commands";
import { Icon } from "./ui/Icon";
import { openSettings, closeSettings, settingsPage, type SettingsPage } from "./settings/window";
import { StatusIndicators } from "./host/components/StatusIndicators";
import { NotificationCenter, notify } from "./host/notifications";
import { BackendError } from "./i18n/errors";
import { ErrorNotice, asDisplayError, type DisplayError } from "./i18n/errors";
import { translate, type MessageValues } from "./i18n";
const t = (key: string, values?: MessageValues) => translate("host", key, values);
import { useLocale } from "./i18n";
import { SetupGate } from "./startup/SetupGate";
import { TradingAccountsProvider } from "./host/trading-accounts";
import { MarketProvider } from "./host/market";
import { useCallback, useEffect, useRef, useState } from "react";
import { WindowFrame } from "@asterion/workbench/components/WindowFrame";
import { Workbench } from "@asterion/workbench/workspace/Workbench";
import { usePreferences } from "./ui/preferences";
import { request, pollSnapshot, type Snapshot } from "@asterion/desktop-bridge/client";
import { Settings } from "./settings/Settings";
import { terminalFeatures, workspaces } from "./features";
import { createWorkspaceDrafts, WorkspaceDraftScope } from "./host/workspace/drafts";
import {
  modules,
  initialNavigation,
  currentRoute,
  navigationLocation,
  selectModule,
  selectView,
} from "./host/navigation";
import type { TerminalContext } from "../features/contract";
import "./ui/theme/style.css";
import "./terminal.css";
import "./host/workspace/workbench.css";
import "./settings/settings.css";
import "./ui/workflows.css";
// One line for every CTP account: how many may send orders, how many are
// connected read-only, and how many need attention.
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
  useLocale();
  usePreferences();
  const [snapshot, setSnapshot] = useState<Snapshot | null>(null);
  const [drafts] = useState(createWorkspaceDrafts);
  const [navigation, setNavigation] = useState(() => {
    try {
      return initialNavigation(JSON.parse(localStorage.getItem("asterion.navigation") ?? "null"));
    } catch {
      return initialNavigation();
    }
  });
  const route = currentRoute(navigation);
  const view = route.view;
  const viewsRef = useRef<HTMLElement>(null);
  // Lab leaving the ready state, lost log lines and a failed background
  // refresh are reported when they begin.
  const reported = useRef({ lab: false, logs: 0, refresh: false });
  useEffect(() => {
    // The main window reports them; the settings window reads the same snapshot.
    if (settingsWindow || !snapshot) return;
    const before = reported.current;
    const lab = snapshot.lab?.state === "ready";
    if (before.lab && !lab)
      notify(new BackendError("service_restarted", `lab: ${snapshot.lab?.error}`), {
        severity: "warning",
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
    viewsRef.current?.querySelector('[aria-current="page"]')?.scrollIntoView({
      block: "nearest",
      inline: "nearest",
    });
  }, [navigation.module, view]);
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
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  const generation = useRef(0);
  // Commands in flight. A cancel, a strategy stop or a revoke may run beside
  // another command.
  const commands = useRef(0);
  const revision = useRef<number | undefined>(undefined);
  // Every full snapshot carries the core's revision and the time the core last
  // refreshed it.
  const accept = useCallback((next: Snapshot) => {
    if (next.revision < (revision.current ?? 0)) return;
    setSnapshot(next);
    setError("");
    revision.current = next.revision;
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
      const module = modules[Number(event.key) - 1];
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
  const polling = !!snapshot;
  const pollInFlight = useRef(false);
  useEffect(() => {
    // Polls read the core's published snapshot by revision; an unchanged
    // revision returns no state. Hidden windows stop polling. A command in
    // flight does not.
    if (!visible || !polling) return;
    let cancelled = false;
    const timer = window.setInterval(async () => {
      // Keep one read per window, including across visibility changes.
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
  }, [visible, polling, accept]);
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
    navigate: (id, options) => setNavigation(previous => selectView(previous, id, options)),
    openSettings: showSettings,
    trade,
  };
  // The selected built-in feature uses the application context.
  const currentFeature = terminalFeatures.find(feature =>
    feature.workspaces.some(item => item.id === view),
  );
  const current = currentFeature?.workspaces.find(item => item.id === view);
  const Panel = current?.component;
  const HeaderTools = current?.headerTools;
  const WorkspaceToolbar = current?.toolbar;
  const panelContext = context;
  return (
    <WindowFrame
      title={settingsWindow ? t("设置") + " — Asterion Terminal" : t("星枢 · Asterion Terminal")}
    >
      {settingsWindow ? (
        <Settings
          initialPage={settingsPage(new URLSearchParams(location.search).get("category"))}
          trade={trade}
          snapshot={snapshot}
          error={error}
          busy={busy}
        />
      ) : (
        <TradingAccountsProvider
          ids={(snapshot?.trading_connections ?? []).flatMap(item =>
            "error" in item ? [] : [item.id],
          )}
          active={visible}
          protocol={snapshot?.trading_protocol}
        >
          <MarketProvider active={visible}>
            <Workbench
              title={t(route.module.title)}
              showToolbar={!current?.hideToolbar && !!WorkspaceToolbar}
              navigation={modules.map((item, index) => ({
                id: item.id,
                title: t(item.title),
                icon: <Icon name={item.icon} />,
                active: item.id === navigation.module,
                shortcut: `⌘${index + 1} / Ctrl+${index + 1}`,
                select: () => setNavigation(previous => selectModule(previous, item.id)),
              }))}
              rail={
                <button
                  aria-label={t("设置")}
                  title={`${t("设置")} · ⌘, / Ctrl+,`}
                  onClick={() => showSettings()}
                >
                  <Icon name="settings" />
                </button>
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
                  {(route.module.views.length > 1 || HeaderTools) && (
                    <div className="module-header">
                      {route.module.views.length > 1 && (
                        <nav ref={viewsRef} className="module-views" aria-label={t("子模块视图")}>
                          {route.module.views.map(id => (
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
                      {HeaderTools && (
                        <div className="module-tools">
                          <WorkspaceDraftScope store={drafts} namespace={current!.id}>
                            <HeaderTools {...panelContext} />
                          </WorkspaceDraftScope>
                        </div>
                      )}
                    </div>
                  )}
                  <main className="terminal-business">
                    {Panel && currentFeature && (
                      <WorkspaceDraftScope store={drafts} namespace={current!.id}>
                        <Panel {...panelContext} />
                      </WorkspaceDraftScope>
                    )}
                  </main>
                </div>
              }

              status={
                <>
                  <StatusIndicators
                    snapshot={snapshot}
                    openMarketSettings={() => showSettings("market")}
                    openMarket={() => context.navigate("workspace.market")}
                    openHistorySettings={() => showSettings("history")}
                    openTradingSettings={() => showSettings("trading")}
                    openTrading={() => context.navigate("workspace.trading")}
                  />
                  <span className="panel-spacer" />
                  <NotificationCenter />
                  {error && <span className="status-updated">{t("状态待确认")}</span>}
                </>
              }
            />
          </MarketProvider>
        </TradingAccountsProvider>
      )}
    </WindowFrame>
  );
}
