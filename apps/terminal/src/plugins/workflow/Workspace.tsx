import { tradingRequests } from "../trading/Account";
import { marketRequests } from "../market/live";
import { roleRequests } from "./requests";
import { useRequestClient } from "../../api/useRequestClient";
import { dataRequests, researchRequests, taskRequests } from "./requests";
import { invoke } from "@tauri-apps/api/core";
import { useEffect, useMemo, useState, type CSSProperties } from "react";
import { request } from "../../api/client";
import { useConnection } from "../../api/useConnection";
import { Connection } from "../../components/Connection";
import { ServiceStatus } from "../../components/ServiceStatus";
import { nativeDesktop } from "../../deployment/desktop";
import { dashboardContributions, distribution, panels, workspaces } from "../../distribution";
import { PanelHost } from "../../extensions/PanelHost";
import { usePreferences } from "../appearance/preferences";
import { openSettings } from "../../settings/window";
import { PanelDivider } from "../../workspace/PanelDivider";
import { Workbench } from "../../workspace/Workbench";
import { type Bar, type Snapshot } from "../data/client";
import { Inspector } from "../data/Inspector";
import type { Job, TaskView } from "../tasks/public";
import { TaskDock } from "../tasks/TaskDock";
import { createCommands, type CommandContext } from "./commands";
import { layoutContract } from "./layout";
import type { View } from "./navigation";

import { Startup } from "../../startup/Startup";
import { useWindowLayout } from "../../workspace/useWindowLayout";
import { AccountEntry } from "../identity/AccountEntry";
import { accountRequest, clearAccountSession } from "../identity/client";
import { TerminalSecurity } from "../identity/TerminalSecurity";
import { useDesktopAccount } from "../identity/useDesktopAccount";
import { MarketBoard } from "../market/MarketBoard";

export function Workspace() {
  const commands = useMemo(() => createCommands(workspaces), []);
  const {
    layout,
    setLayout,
    ready: layoutReady,
    error: layoutError,
    flush,
  } = useWindowLayout(layoutContract);
  const view = layout.view;
  const [dataTarget, setDataTarget] = useState<{
    versionId: string;
    reportId: string;
  } | null>(null);
  const [externalRefill, setExternalRefill] = useState<{
    reportId: string;
    jobs: Job[];
  } | null>(null);
  const setView = (view: View) =>
    setLayout((l) => ({ ...l, view: l.kind === "chart" ? "市场" : view }));
  const connection = useConnection();
  const tradingApi = useRequestClient(connection.token, connection.connected, tradingRequests, "trading");
  const marketApi = useRequestClient(connection.token, connection.connected, marketRequests, "market");
  const rolesApi = useRequestClient(connection.token, connection.connected, roleRequests, "roles");
  const dataApi = useRequestClient(
    connection.token,
    connection.connected,
    dataRequests,
    "data",
  );
  const researchApi = useRequestClient(
    connection.token,
    connection.connected,
    researchRequests,
    "research",
  );
  const taskApi = useRequestClient(
    connection.token,
    connection.connected,
    taskRequests,
    "tasks",
  );
  const { account, setAccount, restoring } = useDesktopAccount(
    connection.token,
  );
  const section = layout.section;
  const [referenceRules, setReferenceRules] = useState(false);
  const workspace = workspaces.find((entry) => entry.title === view);
  const sectionEntry =
    workspace?.sections.find((entry) => entry.title === section) ??
    workspace?.sections[0];
  const activeSection = sectionEntry?.title;
  const activePanel = sectionEntry ? panels.get(sectionEntry.panel) : undefined;
  const [startupComplete, setStartupComplete] = useState(!nativeDesktop);
  const [catalogError, setCatalogError] = useState("");
  const [refreshedAt, setRefreshedAt] = useState<number | null>(null);
  const [barsLoading, setBarsLoading] = useState(false);
  const [barsError, setBarsError] = useState("");
  const [securityEpoch, setSecurityEpoch] = useState(0);
  useEffect(() => {
    const refresh = () => setSecurityEpoch((n) => n + 1);
    window.addEventListener("asterion:unlocked", refresh);
    return () => window.removeEventListener("asterion:unlocked", refresh);
  }, []);
  useEffect(() => {
    if (connection.connected) setStartupComplete(true);
  }, [connection.connected]);
  const preferences = usePreferences();
  const [jobs, setJobs] = useState<Job[]>([]);
  const [snapshots, setSnapshots] = useState<Snapshot[]>([]);
  const [selected, setSelected] = useState<Snapshot>();
  const [bars, setBars] = useState<Bar[]>([]);
  const contract = layout.contract;
  const setContract = (contract: string) =>
    setLayout((l) => ({ ...l, contract }));
  const [search, setSearch] = useState("");
  const [error, setError] = useState("");
  useEffect(() => {
    if (nativeDesktop && layoutReady && account)
      void invoke("desktop_workspace_ready").catch((e) => setError(String(e)));
  }, [layoutReady, account]);
  useEffect(() => {
    if (!layoutReady || !snapshots.length) return;
    const next =
      snapshots.find((s) => s.id === layout.snapshot) ?? snapshots[0];
    setSelected((previous) => (previous?.id === next.id ? previous : next));
    if (!layout.snapshot)
      setLayout((l) => (l.snapshot ? l : { ...l, snapshot: next.id }));
  }, [layoutReady, layout.snapshot, snapshots]);
  useEffect(() => {
    if (!account && nativeDesktop) {
      setJobs([]);
      setRefreshedAt(null);
      setCatalogError("");
      setSnapshots([]);
      setSelected(undefined);
      setBars([]);
    }
  }, [account]);
  const commandContext = (): CommandContext => ({
    locked: document.documentElement.dataset.locked === "true",
    chartWindow: layout.kind === "chart",
    setView,
    toggleTasks: () => setLayout((l) => ({ ...l, tasks: !l.tasks })),
    openSettings,
  });
  const executeCommand = (id: string) => {
    void commands
      .execute(id, commandContext())
      .catch((e) => setError(String(e)));
  };
  useEffect(() => {
    const key = (event: KeyboardEvent) => {
      if (
        !(event.metaKey || event.ctrlKey) ||
        event.altKey ||
        event.shiftKey ||
        event.repeat
      )
        return;
      const command = commands.shortcut(event.key);
      if (!command || !command.enabled(commandContext())) return;
      event.preventDefault();
      executeCommand(command.id);
    };
    window.addEventListener("keydown", key);
    return () => window.removeEventListener("keydown", key);
  });
  useEffect(() => {
    if (!connection.token || (nativeDesktop && !account)) return;
    let active = true;
    const refresh = async () => {
      try {
        const [j, s] = await Promise.all([
          request<Job[]>("/jobs", connection.token),
          request<Snapshot[]>("/snapshots", connection.token),
        ]);
        if (active) {
          setJobs(j);
          setSnapshots(s);
          setCatalogError("");
          setRefreshedAt(Date.now());
        }
      } catch (e) {
        if (active) setCatalogError(String(e));
      }
    };
    void refresh();
    const timer = setInterval(refresh, 2500);
    return () => {
      active = false;
      clearInterval(timer);
    };
  }, [connection.token, account, securityEpoch]);
  useEffect(() => {
    setBars([]);
    setBarsError("");
    setBarsLoading(false);
    if (!workspace?.objectTools || !selected || (nativeDesktop && !account)) return;
    let active = true;
    setBarsLoading(true);
    request<Bar[]>(`/snapshots/${selected.id}/bars`, connection.token)
      .then((rows) => {
        if (active) {
          setBars(rows);
          setLayout((latest) =>
            rows.some((row) => row.contract === latest.contract)
              ? latest
              : { ...latest, contract: rows[0]?.contract ?? "" },
          );
        }
      })
      .catch((e) => {
        if (active) { setBarsError(String(e)); setError(String(e)); }
      })
      .finally(() => { if (active) setBarsLoading(false); });
    return () => {
      active = false;
    };
  }, [selected, connection.token, account, securityEpoch, workspace?.objectTools]);
  async function popout(kind: "workspace" | "chart" = "workspace") {
    try {
      await flush();
      if (nativeDesktop)
        await invoke("desktop_workspace_open", {
          id: `workspace-${crypto.randomUUID()}`,
          kind,
        });
      else
        window.open(`/?view=${encodeURIComponent(view)}`, "_blank", "noopener");
    } catch (e) {
      setError(String(e));
    }
  }
  async function merge() {
    try {
      await flush();
      await invoke("desktop_workspace_merge");
    } catch (e) {
      setError(String(e));
    }
  }
  function select(snapshot: Snapshot) {
    setSelected(snapshot);
    setLayout((l) => ({
      ...l,
      view: "市场",
      snapshot: snapshot.id,
      contract: snapshot.manifest.contracts[0] ?? "",
    }));
  }
  const activeJobs = jobs.filter((j) =>
    ["QUEUED", "RUNNING"].includes(j.state),
  ).length;
  const contractBars = useMemo(
    () => bars.filter((b) => b.contract === contract),
    [bars, contract],
  );
  const last = contractBars.at(-1);
  if (!layoutReady)
    return (
      <div className="startup-screen">{layoutError || "正在恢复工作区…"}</div>
    );
  if (!startupComplete)
    return (
      <Startup
        step={connection.phase}
        error={connection.error || catalogError}
        retry={() => {
          setCatalogError("");
          void connection.connect();
        }}
      />
    );
  if (restoring && nativeDesktop)
    return <div className="startup-screen">正在恢复桌面会话…</div>;
  if (!account && nativeDesktop)
    return <AccountEntry token={connection.token} onEnter={setAccount} />;
  return (
    <TerminalSecurity token={connection.token} account={account}>
      <Workbench
        title={view}
        navigation={workspaces.map(({ title, icon, id }, index) => ({
          id,
          title,
          icon,
          active: view === title,
          disabled: layout.kind === "chart" && title !== "市场",
          shortcut: `⌘${index + 1}`,
          select: () => executeCommand(id),
        }))}
        sections={(workspace?.sections ?? []).map(({ title }) => ({
          id: title,
          title,
          active: activeSection === title,
          select: () => setLayout((l) => ({ ...l, section: title })),
        }))}
        rail={
          <>
            {" "}
            {nativeDesktop && (
              <button
                aria-label="锁定终端"
                title="锁定终端 · ⌘⇧L"
                onClick={() => window.dispatchEvent(new Event("asterion:lock"))}
              >
                ▣
              </button>
            )}
            {account && (
              <button
                aria-label="退出账号"
                title={account.email}
                onClick={() =>
                  accountRequest("/logout", connection.token, {})
                    .then(() => {
                      clearAccountSession();
                      setAccount(undefined);
                      setJobs([]);
                      setSnapshots([]);
                      setSelected(undefined);
                      setBars([]);
                    })
                    .catch((e) => setError(String(e)))
                }
              >
                ⇥
              </button>
            )}
            <button
              aria-label="新建独立窗口"
              title="新建独立窗口"
              onClick={() => void popout()}
            >
              ↗
            </button>
            <button
              aria-label="设置"
              title="设置 ⌘,"
              onClick={() => executeCommand("terminal.settings")}
            >
              ⚙
            </button>
          </>
        }
        toolbar={
          <>
            {" "}
            {workspace?.objectTools && <>
            <span className="mono current-object">
              {contract || "未选择合约"}
            </span>
            {selected && (
              <span className="subtle">快照 {selected.id.slice(0, 8)}</span>
            )}
            <span className="panel-spacer" />
            {nativeDesktop && (
              <>
                <select
                  aria-label="窗口联动组"
                  value={layout.linkGroup}
                  onChange={(e) => {
                    const group = e.target.value;
                    setLayout((l) => ({ ...l, linkGroup: group }));
                  }}
                >
                  <option value="">不联动</option>
                  {["A", "B", "C"].map((g) => (
                    <option key={g} value={g}>
                      联动 {g}
                    </option>
                  ))}
                </select>
                <button
                  className={layout.locked ? "pressed" : ""}
                  onClick={() =>
                    setLayout((l) => ({ ...l, locked: !l.locked }))
                  }
                >
                  {layout.locked ? "已锁定合约" : "锁定合约"}
                </button>
              </>
            )}
            <button
              className={layout.inspector ? "pressed" : ""}
              onClick={() =>
                setLayout((l) => ({ ...l, inspector: !l.inspector }))
              }
            >
              检查器
            </button>
            </>}
            <span className="panel-spacer" />
            <button
              className={layout.tasks ? "pressed" : ""}
              onClick={() => executeCommand("terminal.tasks")}
            >
              任务{" "}
              {activeJobs > 0 && <span className="count">{activeJobs}</span>}
            </button>
          </>
        }
        notices={
          <>
            {" "}
            {!connection.connected && !nativeDesktop && (
              <Connection onConnect={connection.setToken} />
            )}
            {(error || layoutError || (!nativeDesktop && connection.error)) && (
              <div className="alert" role="alert">
                {error || layoutError || connection.error}
                <button onClick={() => setError("")} aria-label="关闭提示">
                  ×
                </button>
              </div>
            )}
            {nativeDesktop && connection.starting && (
              <div className="notice">正在准备本机工作台…</div>
            )}
            {nativeDesktop && !connection.starting && !connection.connected && (
              <div className="notice">
                本机服务未连接
                <span className="panel-spacer" />
                <button onClick={() => executeCommand("terminal.settings")}>
                  打开设置
                </button>
              </div>
            )}
          </>
        }
        body={
          <>
            {" "}
            <div
              style={
                { "--market-ratio": `${layout.marketRatio}%` } as CSSProperties
              }
              className={`workspace-body ${view === "市场" && activeSection === "行情全景" ? "market-layout" : ""} ${workspace?.objectTools && layout.inspector ? "with-inspector" : ""} dock-${layout.dock}`}
            >
              {view === "市场" && activeSection === "行情全景" && (
                <MarketBoard
                  snapshots={snapshots}
                  selected={selected}
                  onSnapshot={select}
                  bars={bars}
                  contract={contract}
                  onContract={setContract}
                  search={search}
                  onSearch={setSearch}
                />
              )}
              {view === "市场" && activeSection === "行情全景" && (
                <PanelDivider
                  horizontal={layout.dock === "top" || layout.dock === "bottom"}
                  reverse={layout.dock === "right" || layout.dock === "bottom"}
                  ratio={layout.marketRatio}
                  onChange={(marketRatio) =>
                    setLayout((l) => ({ ...l, marketRatio }))
                  }
                />
              )}
              <main className="business-panel">
                {activePanel ? (
                  <PanelHost
                    key={account?.email || "local-development"}
                    registry={panels}
                    activeId={activePanel.id}
                    context={{
                      empty: {},
                      overview: {
                        marketApi,
                        tradingApi,
                        widgets: dashboardContributions,
                        storageKey: `asterion.dashboard:${account?.email || "local-development"}`,
                        refresh: () => setSecurityEpoch(n => n + 1),
                        openTrading: () => setView("交易"),
                        openIntelligence: () => setView("情报"),
                        jobs,
                        connected: connection.connected,
                        catalogError, refreshedAt,
                        taskViews: distribution.extensions.all()
                          .filter((entry) => entry.point === "tasks.views")
                          .map((entry) => entry.value as TaskView),
                        openData: () => setLayout((l) => ({ ...l, view: "数据", section: "数据同步" })),
                        openTasks: () => setLayout((l) => ({ ...l, tasks: true })),
                      },
                      trading: { api: tradingApi, connected: connection.connected },
                      market: {
                        layout: {
                          detached: layout.detached,
                          kind: layout.kind,
                          dock: layout.dock,
                          viewport: layout.viewport,
                        },
                        preferences,
                        bars,
                        contractBars,
                        contract,
                        setContract,
                        selected,
                        last,
                        merge,
                        popout,
                        openData: () => setView("数据"),
                        setDock: (dock) => setLayout((l) => ({ ...l, dock })),
                        saveViewport: (range) =>
                          setLayout((l) =>
                            l.contract !== contract
                              ? l
                              : {
                                  ...l,
                                  viewport: {
                                    ...range,
                                    snapshot: selected?.id ?? "",
                                    contract,
                                  },
                                },
                          ),
                      },
                      roles: { api: rolesApi, connected: connection.connected },
                      data: {
                        connection: {
                          api: dataApi,
                          connected: connection.connected,
                        },
                        select,
                        referenceRules,
                        showReferenceRules: setReferenceRules,
                        dataTarget,
                        clearTarget: () => setDataTarget(null),
                        openResearch: () => setView("研究"),
                        openSync: () =>
                          setLayout((l) => ({ ...l, section: "数据同步" })),
                        openCatalog: () =>
                          setLayout((l) => ({ ...l, section: "数据集" })),
                        prepared: (versionId, reportId) => {
                          setDataTarget({ versionId, reportId });
                          setLayout((l) => ({ ...l, section: "数据集" }));
                        },
                        submitted: (job) => {
                          setJobs((j) => [
                            job,
                            ...j.filter((item) => item.id !== job.id),
                          ]);
                          setLayout((l) => ({ ...l, tasks: true }));
                        },
                        refilled: (submitted, reportId) => {
                          if (dataTarget)
                            setExternalRefill({
                              reportId: reportId || dataTarget.reportId,
                              jobs: submitted,
                            });
                          setJobs((previous) => [
                            ...submitted,
                            ...previous.filter(
                              (job) =>
                                !submitted.some((value) => value.id === job.id),
                            ),
                          ]);
                          setLayout((l) => ({ ...l, tasks: true }));
                        },
                      },
                      research: {
                        connection: {
                          api: researchApi,
                          connected: connection.connected,
                        },
                        accountEmail: account?.email || "local-development",
                        externalRefill,
                        inspectData: (versionId, reportId) => {
                          setDataTarget({ versionId, reportId });
                          setLayout((l) => ({
                            ...l,
                            view: "数据",
                            section: "数据集",
                          }));
                        },
                        submitted: (job) => {
                          setJobs((j) => [
                            job,
                            ...j.filter((item) => item.id !== job.id),
                          ]);
                          setLayout((l) => ({ ...l, tasks: true }));
                        },
                      },
                    }}
                  />
                ) : (
                  <div className="panel-empty">
                    工作区插件不可用；布局记录已保留，请选择可用工作区。
                  </div>
                )}
              </main>
              {workspace?.objectTools && layout.inspector && (
                <Inspector
                  snapshot={selected}
                  onClose={() => setLayout((l) => ({ ...l, inspector: false }))}
                />
              )}
            </div>
          </>
        }
        dock={
          <>
            {" "}
            {layout.tasks && (
              <TaskDock
                api={taskApi}
                jobs={jobs}
                height={layout.taskHeight}
                onResize={(height) =>
                  setLayout((l) => ({ ...l, taskHeight: height }))
                }
                onClose={() => setLayout((l) => ({ ...l, tasks: false }))}
                views={distribution.extensions
                  .all()
                  .filter((entry) => entry.point === "tasks.views")
                  .map((entry) => entry.value as TaskView)}
                onSubmitted={(job) =>
                  setJobs((previous) => [
                    job,
                    ...previous.filter((item) => item.id !== job.id),
                  ])
                }
                onError={setError}
                onCancel={(id) =>
                  request(`/jobs/${id}/cancel`, connection.token, {}).catch(
                    (e) => setError(String(e)),
                  )
                }
              />
            )}
          </>
        }
        status={
          <>
            {" "}
            <ServiceStatus
              token={connection.token}
              connected={connection.connected}
            />
            <span className="status-divider" />
            <span>{activeJobs} 项任务执行中</span>
            <span className="panel-spacer" />
            <span role="status" title={catalogError || (refreshedAt ? `状态读取于 ${new Date(refreshedAt).toLocaleTimeString("zh-CN", { hour12: false })}` : "正在读取状态")}>
              {!connection.connected ? "数据：连接中断" : catalogError ? "状态待确认" : refreshedAt === null ? "正在读取状态…" : `状态读取于 ${new Date(refreshedAt).toLocaleTimeString("zh-CN", { hour12: false })}`}
            </span>
            <span className="status-divider" />
            <span>交易未连接</span>
          </>
        }
      />
    </TerminalSecurity>
  );
}
