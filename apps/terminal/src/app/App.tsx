import { DataSync } from "../panels/DataSync";
import { useEffect, useMemo, useState, type CSSProperties } from "react";
import { invoke } from "@tauri-apps/api/core";
import { PanelDivider } from "../workspace/PanelDivider";
import { request, type Job, type Snapshot, type Bar } from "../api/client";
import { useConnection } from "../api/useConnection";
import { Connection } from "../components/Connection";
import { nativeDesktop } from "../deployment/desktop";
import { views, type View } from "../workspace/layout";
import { openSettings } from "../settings/window";
import { usePreferences } from "../settings/preferences";
import { ReferenceCatalog } from "../panels/ReferenceCatalog";
import { Chart } from "../panels/Chart";
import { DatasetCatalog } from "../panels/DatasetCatalog";
import { Inspector } from "../panels/Inspector";
import { SnapshotTable } from "../panels/SnapshotTable";
import { TaskDock } from "../panels/TaskDock";

import { useDesktopAccount } from "../account/useDesktopAccount";
import { useWindowLayout } from "../workspace/useWindowLayout";
import { TerminalSecurity } from "../account/TerminalSecurity";
import { AccountEntry } from "../account/AccountEntry";
import { accountRequest, clearAccountSession } from "../account/client";
import { Startup } from "../startup/Startup";
import { MarketBoard } from "../panels/MarketBoard";

const subviews: Record<View, string[]> = {
  总览: ["工作概览"],
  市场: ["行情全景", "历史图表"],
  数据: ["数据同步", "数据集", "合约资料"],
  研究: ["实验与运行"],
  交易: ["持仓与委托"],
  情报: ["事件与报告"],
};
const icons = ["▦", "⌁", "▤", "⌘", "⇄", "◎"];

export function App() {
  const {
    layout,
    setLayout,
    ready: layoutReady,
    error: layoutError,
    flush,
  } = useWindowLayout();
  const view = layout.view;
  const setView = (view: View) =>
    setLayout((l) => ({ ...l, view: l.kind === "chart" ? "市场" : view }));
  const connection = useConnection();
  const { account, setAccount, restoring } = useDesktopAccount(
    connection.token,
  );
  const section = layout.section;
  const [referenceRules, setReferenceRules] = useState(false);
  const activeSection = subviews[view].includes(section)
    ? section
    : subviews[view][0];
  const [startupComplete, setStartupComplete] = useState(!nativeDesktop);
  const [catalogError, setCatalogError] = useState("");
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
      setSnapshots([]);
      setSelected(undefined);
      setBars([]);
    }
  }, [account]);
  useEffect(() => {
    const key = (event: KeyboardEvent) => {
      if (document.documentElement.dataset.locked === "true") return;
      if (!(event.metaKey || event.ctrlKey)) return;
      if (event.key === ",") {
        event.preventDefault();
        void openSettings().catch((e) => setError(String(e)));
      }
      if (event.key.toLowerCase() === "j") {
        event.preventDefault();
        setLayout((l) => ({ ...l, tasks: !l.tasks }));
      }
      if (/^[1-6]$/.test(event.key)) {
        event.preventDefault();
        setView(views[Number(event.key) - 1]);
      }
    };
    window.addEventListener("keydown", key);
    return () => window.removeEventListener("keydown", key);
  }, []);
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
    if (!selected || (nativeDesktop && !account)) return;
    let active = true;
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
        if (active) setError(String(e));
      });
    return () => {
      active = false;
    };
  }, [selected, connection.token, account, securityEpoch]);
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
  const filtered = snapshots.filter((s) =>
    s.manifest.contracts.join(",").toLowerCase().includes(search.toLowerCase()),
  );
  const activeJobs = jobs.filter((j) =>
    ["QUEUED", "RUNNING"].includes(j.state),
  ).length;
  const contractBars = useMemo(
    () => bars.filter((b) => b.contract === contract),
    [bars, contract],
  );
  const last = contractBars.at(-1);
  if (!layoutReady && nativeDesktop)
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
      <div className="terminal">
        <aside className="activity-rail">
          <div className="rail-brand" title="Asterion Terminal">
            ✧
          </div>
          <nav aria-label="业务工作区" className="workspace-tabs">
            {views.map((v, i) => (
              <button
                key={v}
                disabled={layout.kind === "chart" && v !== "市场"}
                aria-current={view === v ? "page" : undefined}
                className={view === v ? "active" : ""}
                onClick={() => setView(v)}
                title={`⌘${i + 1}`}
              >
                <span aria-hidden="true">{icons[i]}</span>
                {v}
              </button>
            ))}
          </nav>
          <div className="rail-actions">
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
              onClick={() => openSettings().catch((e) => setError(String(e)))}
            >
              ⚙
            </button>
          </div>
        </aside>
        <div className="terminal-content">
          <div className="workspace-toolbar">
            <h1>{view}</h1>
            <span className="toolbar-divider" />
            <nav className="section-tabs" aria-label="业务内分类">
              {subviews[view].map((name) => (
                <button
                  key={name}
                  className={activeSection === name ? "active" : ""}
                  aria-current={activeSection === name ? "page" : undefined}
                  onClick={() => setLayout((l) => ({ ...l, section: name }))}
                >
                  {name}
                </button>
              ))}
            </nav>
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
            <button
              className={layout.tasks ? "pressed" : ""}
              onClick={() => setLayout((l) => ({ ...l, tasks: !l.tasks }))}
            >
              任务{" "}
              {activeJobs > 0 && <span className="count">{activeJobs}</span>}
            </button>
          </div>
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
              <button
                onClick={() => openSettings().catch((e) => setError(String(e)))}
              >
                打开设置
              </button>
            </div>
          )}
          <div
            style={
              { "--market-ratio": `${layout.marketRatio}%` } as CSSProperties
            }
            className={`workspace-body ${view === "市场" && activeSection === "行情全景" ? "market-layout" : ""} ${layout.inspector ? "with-inspector" : ""} dock-${layout.dock}`}
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
              {view === "市场" && layout.detached && (
                <div className="table-empty-state">
                  <span>图表已在独立窗口打开</span>
                  <button onClick={merge}>归并图表</button>
                </div>
              )}
              {view === "市场" && !layout.detached && (
                <>
                  <div className="panel-heading">
                    <h2>历史 K 线</h2>
                    <span className="panel-spacer" />
                    {bars.length > 0 && (
                      <select
                        aria-label="合约"
                        value={contract}
                        onChange={(e) => setContract(e.target.value)}
                      >
                        {[...new Set(bars.map((b) => b.contract))].map((c) => (
                          <option key={c}>{c}</option>
                        ))}
                      </select>
                    )}
                    {layout.kind !== "chart" && (
                      <select
                        aria-label="行情面板停靠位置"
                        value={layout.dock}
                        onChange={(e) =>
                          setLayout((l) => ({
                            ...l,
                            dock: e.target.value as typeof l.dock,
                          }))
                        }
                      >
                        <option value="left">行情居左</option>
                        <option value="right">行情居右</option>
                        <option value="top">行情居上</option>
                        <option value="bottom">行情居下</option>
                      </select>
                    )}
                    <small>
                      {last
                        ? selected?.manifest.frequency === "1d"
                          ? "历史日线 · 按交易日标记"
                          : "历史行情"
                        : "未加载"}
                    </small>
                    {nativeDesktop &&
                      (layout.kind === "chart" ? (
                        <button onClick={merge}>归并图表</button>
                      ) : (
                        <button onClick={() => void popout("chart")}>
                          拆出图表
                        </button>
                      ))}
                  </div>
                  <div className="quote-strip">
                    <b className="mono">{contract || "—"}</b>
                    {(["open", "high", "low", "close", "volume"] as const).map(
                      (field, i) => (
                        <span key={field}>
                          <small>{["开", "高", "低", "收", "量"][i]}</small>
                          <strong>
                            {last
                              ? Number(last[field]).toLocaleString("zh-CN", {
                                  maximumFractionDigits: 4,
                                })
                              : "—"}
                          </strong>
                        </span>
                      ),
                    )}
                  </div>
                  <div className="chart-panel">
                    {contractBars.length ? (
                      <Chart
                        bars={contractBars}
                        colors={preferences.colors}
                        viewport={
                          layout.viewport?.snapshot === selected?.id &&
                          layout.viewport?.contract === contract
                            ? layout.viewport
                            : null
                        }
                        onViewport={(range) =>
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
                          )
                        }
                      />
                    ) : (
                      <div className="chart-placeholder">
                        <div>
                          <span>⌁</span>
                          <h2>尚未选择行情</h2>
                          <p>同步或导入历史数据后选择合约查看走势</p>
                          <button
                            onClick={() =>
                              layout.kind === "chart"
                                ? void merge()
                                : setView("数据")
                            }
                          >
                            {layout.kind === "chart"
                              ? "返回原工作台"
                              : "获取数据"}
                          </button>
                        </div>
                      </div>
                    )}
                  </div>
                  <div className="panel-footnote">
                    <span>
                      {selected
                        ? `${selected.manifest.start} — ${selected.manifest.end}`
                        : "数据时间 —"}
                    </span>
                    <span className="panel-spacer" />
                    <span>
                      {bars.length
                        ? `${bars.length} 行 · 最多展示 1,000 行`
                        : "历史数据"}
                    </span>
                  </div>
                </>
              )}
              {view === "数据" && activeSection === "合约资料" && (
                <>
                  <div className="panel-heading">
                    <button
                      className={!referenceRules ? "pressed" : ""}
                      onClick={() => setReferenceRules(false)}
                    >
                      基础资料
                    </button>
                    <button
                      className={referenceRules ? "pressed" : ""}
                      onClick={() => setReferenceRules(true)}
                    >
                      规则版本
                    </button>
                    <span className="panel-spacer" />
                    <small>合约信息与交易规则</small>
                  </div>
                  {referenceRules ? (
                    <ReferenceCatalog
                      token={connection.token}
                      connected={connection.connected}
                    />
                  ) : (
                    <DatasetCatalog
                      key="contracts"
                      token={connection.token}
                      connected={connection.connected}
                      onSnapshot={select}
                      contractsOnly
                      onSync={() =>
                        setLayout((l) => ({ ...l, section: "数据同步" }))
                      }
                    />
                  )}
                </>
              )}
              {view === "数据" && activeSection === "数据同步" && (
                <DataSync
                  token={connection.token}
                  connected={connection.connected}
                  onBrowse={() =>
                    setLayout((l) => ({ ...l, section: "数据集" }))
                  }
                  onSubmitted={(job) => {
                    setJobs((j) => [job, ...j]);
                    setLayout((l) => ({ ...l, tasks: true }));
                  }}
                />
              )}
              {view === "数据" && activeSection === "数据集" && (
                <DatasetCatalog
                  key="datasets"
                  token={connection.token}
                  connected={connection.connected}
                  onSnapshot={select}
                  onSync={() =>
                    setLayout((l) => ({ ...l, section: "数据同步" }))
                  }
                />
              )}
              {view === "总览" && (
                <>
                  <div className="panel-heading">
                    <h2>当前工作</h2>
                  </div>
                  <div className="summary-line">
                    <span>
                      快照 <b>{snapshots.length}</b>
                    </span>
                    <span>
                      执行中 <b>{activeJobs}</b>
                    </span>
                    <span>
                      异常{" "}
                      <b>{jobs.filter((j) => j.state === "FAILED").length}</b>
                    </span>
                  </div>
                  <div className="panel-heading">
                    <h2>数据目录</h2>
                  </div>
                  <div className="panel-scroll">
                    <SnapshotTable
                      snapshots={filtered}
                      selected={selected?.id}
                      onSelect={select}
                    />
                    {!snapshots.length && (
                      <div className="panel-empty">
                        <span>尚无已发布数据</span>
                        <button onClick={() => setView("数据")}>
                          获取历史数据
                        </button>
                      </div>
                    )}
                  </div>
                </>
              )}
              {["研究", "交易", "情报"].includes(view) && (
                <>
                  <div className="panel-heading">
                    <h2>
                      {view === "研究"
                        ? "实验与运行"
                        : view === "交易"
                          ? "持仓与委托"
                          : "事件与报告"}
                    </h2>
                    <span className="panel-spacer" />
                    <small>
                      {view === "交易" ? "账户 — · 环境：研究" : "未接入"}
                    </small>
                  </div>
                  <div className="table-empty-state">
                    <span>
                      {view === "研究"
                        ? "尚无研究运行"
                        : view === "交易"
                          ? "尚未连接交易账户"
                          : "尚无情报数据"}
                    </span>
                    <p>
                      {view === "研究"
                        ? "正式研究执行尚未开放。"
                        : view === "交易"
                          ? "交易执行尚未开放，当前不能发送委托。"
                          : "情报来源尚未接入。"}
                    </p>
                  </div>
                </>
              )}
            </main>
            {layout.inspector && (
              <Inspector
                snapshot={selected}
                onClose={() => setLayout((l) => ({ ...l, inspector: false }))}
              />
            )}
          </div>
          {layout.tasks && (
            <TaskDock
              jobs={jobs}
              height={layout.taskHeight}
              onResize={(height) =>
                setLayout((l) => ({ ...l, taskHeight: height }))
              }
              onClose={() => setLayout((l) => ({ ...l, tasks: false }))}
              onRetry={(id) => {
                void request<Job>(`/data/jobs/${id}/retry`, connection.token, {
                  command_id: crypto.randomUUID(),
                })
                  .then((job) => setJobs((j) => [job, ...j]))
                  .catch((e) => setError(String(e)));
              }}
              onCancel={(id) =>
                request(`/jobs/${id}/cancel`, connection.token, {}).catch((e) =>
                  setError(String(e)),
                )
              }
            />
          )}
          <footer className="status-bar">
            <span className={connection.connected ? "good" : "muted"}>
              ● {connection.connected ? "后台已连接" : "后台未连接"}
            </span>
            <span className="status-divider" />
            <span>{activeJobs} 项任务执行中</span>
            <span className="panel-spacer" />
            <span>研究环境</span>
            <span className="status-divider" />
            <span>交易未连接</span>
          </footer>
        </div>
      </div>
    </TerminalSecurity>
  );
}
