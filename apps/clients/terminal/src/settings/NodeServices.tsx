import { useEffect, useState } from "react";
import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
import type { NodeStatus, Snapshot, TerminalCommand } from "../bridge/client";
import {
  binding,
  kindLabels,
  location,
  matches,
  serviceStatus,
  runtimeStatus,
  t,
  type Service,
  type ServiceKind,
} from "./service-state";
import { RemoteMachine, loadMachineProfiles, type MachineProfile } from "./RemoteMachine";
import { ServiceActionDialog } from "./ServiceActionDialog";
import { FirewallPreview } from "./FirewallPreview";
import { AgentProgram } from "./AgentProgram";
type Action = "stop" | "restart" | "use" | "update";
type Pending = { node: string; service: string; revision: string; action: Action };
const actionLabels: Record<Action, string> = {
  stop: "停止服务",
  restart: "重启服务",
  use: "切换运行位置",
  update: "更新服务程序",
};
// Trading runs one service per CTP account and is managed on the Trading page.
const kinds: ServiceKind[] = ["market", "data", "task"];
export function NodeServices({
  snapshot,
  busy,
  trade,
}: {
  snapshot: Snapshot | null;
  busy: boolean;
  trade: (method: TerminalCommand, params?: Record<string, unknown>) => Promise<void>;
}) {
  const [view, setView] = useState<"overview" | "machines">("overview");
  const [selected, setSelected] = useState("local");
  const [wizard, setWizard] = useState<MachineProfile | "new" | null>(null);
  const [error, setError] = useState<DisplayError>("");
  const [saved, setSaved] = useState<MachineProfile[]>([]);
  const [pending, setPending] = useState<Pending | null>(null);
  const [deploying, setDeploying] = useState(false);
  const [deployment, setDeployment] = useState({ kind: "task", service: "", port: "" });
  const [deployed, setDeployed] = useState<{ node: string; service: string } | null>(null);
  const nodes = snapshot?.nodes ?? [];
  const node = nodes.find(n => n.id === selected);
  const disabled = busy;
  async function run(method: TerminalCommand, params: Record<string, string> = {}) {
    setError("");
    try {
      await trade(method, params);
      return true;
    } catch (reason) {
      setError(asDisplayError(reason));
      return false;
    }
  }
  useEffect(() => {
    // Startup owns the local monitor. Opening settings only observes the shared
    // snapshot; reconnecting here races commands from the workbench window.
    try {
      setSaved(loadMachineProfiles());
    } catch (reason) {
      setError(asDisplayError(reason));
    }
  }, []);
  function manage(id: string) {
    setSelected(id);
    setView("machines");
    setWizard(null);
    setDeploying(false);
  }
  function propose(action: Action, target: NodeStatus, service: Service) {
    setError("");
    setPending({ action, node: target.id, service: service.id, revision: service.revision });
  }
  const pendingNode = nodes.find(n => n.id === pending?.node);
  const pendingService = pendingNode?.health?.services.find(s => s.id === pending?.service);
  const changed =
    !pendingNode ||
    pendingNode.state !== "online" ||
    !!pendingNode.health?.maintenance ||
    !pendingService ||
    pendingService.revision !== pending?.revision;
  async function confirm() {
    if (!pending || !pendingNode || !pendingService || changed || disabled) return;
    const params = { id: pending.node, service: pending.service };
    let success = false;
    if (pending.action === "use") {
      const kind = pendingService.kind;
      if (kind === "live") return;
      const method: TerminalCommand =
        kind === "market" ? "market.attach" : "node.data_tasks.attach";
      success = await run(method, {
        ...params,
        service: kind === "data" ? pendingService.task_service : pending.service,
      });
    } else if (pending.action === "update")
      success = await run("node.update", { ...params, revision: pending.revision });
    else success = await run("node.action", { ...params, action: pending.action });
    if (success) setPending(null);
  }
  async function deploy() {
    if (!node || node.state !== "online" || node.health?.maintenance) return;
    if (await run("node.deploy", { id: node.id, ...deployment })) {
      setDeployed({ node: node.id, service: deployment.service });
      setDeploying(false);
    }
  }
  function startDeployment(targetId: string) {
    const target = nodes.find(item => item.id === targetId);
    const services = target?.health?.services ?? [];
    let suffix = 1;
    while (services.some(s => s.id === `backtest-factor-${suffix}`)) suffix++;
    let port = 7443;
    while (port === target?.port || services.some(s => s.port === port)) port++;
    setDeployment({
      kind: "task",
      service: `backtest-factor-${suffix}`,
      port: targetId === "local" ? "0" : String(port),
    });
    setDeploying(true);
    setDeployed(null);
  }
  const pendingActive = pendingService ? binding(snapshot, pendingService.kind) : null;
  const watched =
    pendingNode && pendingService && matches(pendingActive, pendingNode, pendingService);
  const tasks =
    watched && pendingService?.kind === "task"
      ? (snapshot?.task_service?.tasks.filter(task =>
          ["queued", "running", "cancel_requested", "publishing"].includes(task.state),
        ) ?? [])
      : [];
  return (
    <section className="deployment-center" aria-label={t("运行与部署")}>
      {!wizard && (
        <>
          <div className="deployment-heading">
            <div>
              <p className="subtle">{t("默认在本机运行。仅在需要时添加远程 Linux。")}</p>
            </div>
            <button
              onClick={() => {
                setWizard("new");
                setView("machines");
                setError("");
              }}
              disabled={disabled}
            >
              {t("添加远程机器")}
            </button>
          </div>
          <div className="deployment-tabs" role="group" aria-label={t("部署视图")}>
            <button
              aria-pressed={view === "overview" && !wizard}
              onClick={() => {
                setView("overview");
                setWizard(null);
              }}
            >
              {t("当前运行位置")}
            </button>
            <button
              aria-pressed={view === "machines" && !wizard}
              onClick={() => {
                setView("machines");
                setWizard(null);
              }}
            >
              {t("机器管理")}
            </button>
          </div>
        </>
      )}
      {error && (
        <p role="alert">
          <ErrorNotice error={error} namespace="host" />
        </p>
      )}
      {wizard ? (
        <RemoteMachine
          key={wizard === "new" ? "new" : wizard.id}
          snapshot={snapshot}
          busy={disabled}
          run={run}
          initial={wizard === "new" ? undefined : wizard}
          onCancel={() => setWizard(null)}
          onSaved={profile =>
            setSaved(previous => [...previous.filter(p => p.id !== profile.id), profile])
          }
          onInstalled={profile => {
            setSaved(loadMachineProfiles());
            manage(profile.id);
            startDeployment(profile.id);
          }}
        />
      ) : view === "overview" ? (
        <>
          <div className="runtime-list" role="list" aria-label={t("当前运行位置")}>
            {kinds.map(kind => {
              const active = binding(snapshot, kind);
              const owner = nodes.find(n =>
                n.health?.services.some(s => s.kind === kind && matches(active, n, s)),
              );
              return (
                <div className="runtime-row" role="listitem" key={kind}>
                  <div>
                    <strong>{t(kindLabels[kind])}</strong>
                    <p className="subtle">{active ? active.service : t("尚未连接")}</p>
                  </div>
                  <div className="runtime-location">
                    <span>{location(active, nodes)}</span>
                    <small className={active && !active.online ? "deployment-warning" : "subtle"}>
                      {active ? runtimeStatus(snapshot, kind) : t("尚未启用")}
                    </small>
                  </div>
                  <button onClick={() => manage(owner?.id ?? (active?.remote ? "" : "local"))}>
                    {t("管理")}
                  </button>
                </div>
              );
            })}
          </div>
          <p className="subtle">
            {t("关闭 Terminal 不会停止服务。部署新服务不会自动切换当前连接。")}
          </p>
          <details>
            <summary>{t("本机运行说明")}</summary>
            <p>
              {t(
                "macOS Terminal 使用当前系统账户自动运行服务管理器，无需 SSH、密钥或机器初始化脚本。打开 CTP 交易账户时，交易服务由本机服务管理器按需启动。",
              )}
            </p>
            <button disabled={disabled} onClick={() => void run("node.local")}>
              {t("检查本机服务管理器")}
            </button>
          </details>
        </>
      ) : (
        <>
          <div className="machine-list" aria-label={t("机器列表")}>
            {nodes.map(item => (
              <button
                key={item.id}
                className="machine-choice"
                aria-pressed={selected === item.id}
                onClick={() => manage(item.id)}
              >
                <strong>{item.id === "local" ? t("本机") : item.id}</strong>
                <span>
                  {t(
                    item.state !== "online" ? "失联" : item.health?.maintenance ? "维护中" : "在线",
                  )}
                </span>
              </button>
            ))}
            {saved
              .filter(p => !nodes.some(n => n.id === p.id))
              .map(profile => (
                <div className="saved-machine" key={profile.id}>
                  <span>
                    {profile.id} · {t("未连接")}
                  </span>
                  <button
                    disabled={disabled}
                    onClick={async () => {
                      if (await run("node.connect", { id: profile.id })) manage(profile.id);
                    }}
                  >
                    {t("连接已安装节点")}
                  </button>
                  <button disabled={disabled} onClick={() => setWizard(profile)}>
                    {t("继续配置")}
                  </button>
                </div>
              ))}
          </div>
          {!node && <p>{t("选择一台机器查看服务，或添加远程机器。")}</p>}
          {node && (
            <>
              <div className="deployment-heading">
                <h3>
                  {node.id === "local"
                    ? t("本机服务")
                    : t("{machine} 的服务", { machine: node.id })}
                </h3>
                <button
                  disabled={disabled || node.state !== "online" || node.health?.maintenance}
                  onClick={() => startDeployment(node.id)}
                >
                  {t("部署服务")}
                </button>
              </div>
              {node.state !== "online" && (
                <p role="alert">{t("节点失联，以下是最后确认的状态。不会自动切换或重发命令。")}</p>
              )}
              {node.id === "local" && (
                <AgentProgram
                  agentProgram={snapshot?.agent_program ?? null}
                  busy={disabled}
                  run={run}
                />
              )}
              <details>
                <summary>{t("机器详情")}</summary>
                <p>
                  {node.host}:{node.port} · {node.health?.os} / {node.health?.arch} ·{" "}
                  {node.health?.version}
                </p>
                <p>{node.error}</p>
                {node.id !== "local" && (
                  <button
                    disabled={disabled}
                    onClick={() => void run("node.disconnect", { id: node.id })}
                  >
                    {t("移除监控")}
                  </button>
                )}
              </details>
              {deployed?.node === node.id && (
                <p role="status">
                  {t("{service} 已部署，当前运行位置未改变。请在服务就绪后选择使用。", {
                    service: deployed.service,
                  })}
                </p>
              )}
              {deploying && (
                <form
                  className="deployment-form"
                  aria-label={t("部署服务")}
                  onSubmit={e => {
                    e.preventDefault();
                    void deploy();
                  }}
                >
                  <fieldset disabled={disabled}>
                    <legend>{t("部署到 {machine}", { machine: node.id })}</legend>
                    <p className="subtle">
                      {t(
                        "数据服务保管历史数据，任务服务调度下载、回测和因子任务；两个服务拥有固定配对的独立目录。",
                      )}
                    </p>
                    {node.id === "local" && (
                      <p>
                        {t(
                          "新增实例拥有独立数据和任务。已有实例完整保留，部署后需显式切换；已有任务不会复制或重跑。",
                        )}
                      </p>
                    )}
                    <label>
                      {t("服务类型")}
                      <select
                        aria-label={t("服务类型")}
                        value={deployment.kind}
                        onChange={e => setDeployment({ ...deployment, kind: e.target.value })}
                      >
                        <option value="task">{t("数据与任务服务")}</option>
                        {node.id !== "local" && <option value="market">{t("实时行情")}</option>}
                      </select>
                    </label>
                    <div className="futures-fields">
                      <label>
                        {t("新服务名称")}
                        <input
                          aria-label={t("新服务名称")}
                          required
                          value={deployment.service}
                          onChange={e => setDeployment({ ...deployment, service: e.target.value })}
                        />
                      </label>
                      {node.id !== "local" && (
                        <label>
                          {t("服务端口")}
                          <input
                            aria-label={t("服务端口")}
                            type="number"
                            min={1}
                            max={65535}
                            required
                            value={deployment.port}
                            onChange={e => setDeployment({ ...deployment, port: e.target.value })}
                          />
                        </label>
                      )}
                    </div>
                    {node.id !== "local" && (
                      <p className="subtle">
                        {t("选择目标机器可用的 TCP 端口。部署前会校验名称、端口和内置程序。")}
                      </p>
                    )}
                    <div className="source-actions">
                      <button type="button" onClick={() => setDeploying(false)}>
                        {t("取消")}
                      </button>
                      <button className="primary" type="submit">
                        {t(node.id === "local" ? "部署服务" : "上传并部署")}
                      </button>
                    </div>
                  </fieldset>
                </form>
              )}
              <div className="managed-services" role="list" aria-label={t("服务列表")}>
                {node.health && (
                  <p className="subtle">
                    {t(
                      "节点工作进程额度：{owned} 已占用 · {reserved} 启动预留 · 上限 {limit}",
                      node.health.worker_capacity,
                    )}
                  </p>
                )}
                {node.health?.resource_budget && (
                  <p className="subtle">
                    {t(
                      "节点准入预算：CPU {cpu}/{cpuLimit} · 内存 {memory}/{memoryLimit} MiB · 磁盘工作 {io}/{ioLimit}",
                      {
                        cpu: node.health.resource_budget.committed.cpu_slots,
                        cpuLimit: node.health.resource_budget.limit.cpu_slots,
                        memory: node.health.resource_budget.committed.memory_mib,
                        memoryLimit: node.health.resource_budget.limit.memory_mib,
                        io: node.health.resource_budget.committed.io_slots,
                        ioLimit: node.health.resource_budget.limit.io_slots,
                      },
                    )}
                  </p>
                )}
                {(node.health?.services ?? []).map(service => {
                  const active = matches(binding(snapshot, service.kind), node, service);
                  const unavailable =
                    disabled || node.state !== "online" || !!node.health?.maintenance;
                  // A trading service is named after the account it serves,
                  // when this Terminal has that account open.
                  const owner = Object.entries(snapshot?.live ?? {}).find(
                    ([, entry]) => entry.connection.session === service.id,
                  )?.[0];
                  const account = snapshot?.ctp_connections?.find(item => item.id === owner)?.name;
                  return (
                    <section
                      className="managed-service"
                      role="listitem"
                      aria-label={service.id}
                      key={service.id}
                    >
                      <div className="deployment-heading">
                        <div>
                          <strong>{account ?? service.id}</strong>
                          <p className="subtle">
                            {t(kindLabels[service.kind])} · {serviceStatus(node, service)}
                            {account && (
                              <>
                                {" · "}
                                <code>{service.id}</code>
                              </>
                            )}
                          </p>
                        </div>
                        {/* Only what applies now: a stopped service starts, a wanted
                            one stops, a running one restarts. */}
                        <div className="managed-service-actions">
                          {!service.desired_running && (
                            <button
                              disabled={
                                unavailable ||
                                service.state === "running" ||
                                service.state === "waiting_capacity" ||
                                service.state === "starting"
                              }
                              onClick={() =>
                                void run("node.action", {
                                  id: node.id,
                                  service: service.id,
                                  action: "start",
                                })
                              }
                            >
                              {t("启动")}
                            </button>
                          )}
                          {service.desired_running && (
                            <button
                              disabled={unavailable || !service.desired_running}
                              onClick={() => propose("stop", node, service)}
                            >
                              {t("停止")}
                            </button>
                          )}
                          {service.state === "running" && (
                            <button
                              disabled={unavailable || service.state !== "running"}
                              onClick={() => propose("restart", node, service)}
                            >
                              {t("重启")}
                            </button>
                          )}
                          {service.kind === "live" ? null : active ? (
                            <span className="deployment-badge">{t("当前使用")}</span>
                          ) : (
                            <button
                              disabled={unavailable || service.state !== "running"}
                              onClick={() => propose("use", node, service)}
                            >
                              {t("使用此服务")}
                            </button>
                          )}
                        </div>
                      </div>
                      <details>
                        <summary>{t("管理服务")}</summary>
                        <p>{t("运行中的工作进程：{count}", { count: service.active_workers })}</p>
                        <details>
                          <summary>{t("程序更新")}</summary>
                          <p>
                            {t(
                              "先停止服务（会中断连接与任务），再更新程序。保留配置与数据，更新后需手动启动。",
                            )}
                          </p>
                          <button
                            disabled={
                              unavailable ||
                              service.state !== "stopped" ||
                              service.desired_running ||
                              !service.revision
                            }
                            onClick={() => propose("update", node, service)}
                          >
                            {t("更新已停止的服务")}
                          </button>
                        </details>
                        {node.id !== "local" && (
                          <details>
                            <summary>{t("端口访问")}</summary>
                            <div className="source-actions">
                              <button
                                disabled={unavailable}
                                onClick={() =>
                                  void run("node.service_firewall", {
                                    id: node.id,
                                    service: service.id,
                                    action: "allow",
                                    token: "",
                                  })
                                }
                              >
                                {t("检查端口放行")}
                              </button>
                              <button
                                disabled={unavailable}
                                onClick={() =>
                                  void run("node.service_firewall", {
                                    id: node.id,
                                    service: service.id,
                                    action: "remove",
                                    token: "",
                                  })
                                }
                              >
                                {t("检查规则撤销")}
                              </button>
                            </div>
                          </details>
                        )}
                        <details>
                          <summary>{t("技术详情")}</summary>
                          <p>
                            PID {service.pid} · {service.port || service.endpoint}
                          </p>
                          <p>
                            {t("自动重启")} {service.restarts}/3
                          </p>
                          <p>{service.directory}</p>
                          <p>{service.error}</p>
                        </details>
                      </details>
                    </section>
                  );
                })}
                {!node.health?.services.length && (
                  <p className="subtle">
                    {t(
                      node.id === "local"
                        ? "暂无服务，请检查本机启动状态。"
                        : "尚未部署服务。添加服务后，本机运行不受影响。",
                    )}
                  </p>
                )}
              </div>
              {snapshot?.firewall_plan?.transport === "agent" &&
                snapshot.firewall_plan.id === node.id && (
                  <FirewallPreview plan={snapshot.firewall_plan} busy={disabled} run={run} />
                )}
            </>
          )}
        </>
      )}
      {pending && (
        <ServiceActionDialog
          title={t(actionLabels[pending.action])}
          busy={busy}
          disabled={
            disabled ||
            changed ||
            (pending.action === "use" && pendingService?.state !== "running") ||
            (pending.action === "update" &&
              (pendingService?.state !== "stopped" || pendingService.desired_running))
          }
          onCancel={() => {
            setPending(null);
            setError("");
          }}
          onConfirm={() => void confirm()}
        >
          <p>
            <strong>
              {pending.node === "local" ? t("本机") : pending.node} / {pending.service}
            </strong>
          </p>
          {changed ? (
            <p role="alert">{t("目标状态已变化，请取消后重新检查。")}</p>
          ) : (
            <>
              {pending.action === "use" ? (
                <>
                  <p>
                    {t("当前：{location} → 目标：{target}", {
                      location: pendingActive ? location(pendingActive, nodes) : t("尚未连接"),
                      target: pending.node === "local" ? t("本机") : pending.node,
                    })}
                  </p>
                  <p>{t("只切换此功能的连接。原服务继续运行，已有数据和任务不会搬到目标机器。")}</p>
                  {pendingService?.kind === "live" && pendingActive && (
                    <p>
                      {t(
                        "将先断开当前账户连接；若目标连接失败，需要手动重新连接。不会重发交易命令。",
                      )}
                    </p>
                  )}
                </>
              ) : pending.action === "update" ? (
                <p>{t("仅更新此服务的程序，保留配置和数据；完成后仍保持停止。")}</p>
              ) : (
                <>
                  <p>{t("此服务的连接会中断，运行中的任务可能中断。其他机器不会被停止。")}</p>
                  <p>
                    {t("当前工作进程：{count}", { count: pendingService?.active_workers ?? 0 })}
                  </p>
                  {tasks.length > 0 && (
                    <p>
                      {t("当前可见的未完成任务：{tasks}", {
                        tasks: tasks.map(task => task.id).join(", "),
                      })}
                    </p>
                  )}
                  {pendingService?.kind === "live" && (
                    <p>{t("停止交易服务不会自动撤销柜台委托，请先在账户中核对委托与持仓。")}</p>
                  )}
                </>
              )}
              {!watched && pending.action !== "use" && (
                <p className="subtle">
                  {t("当前未连接此服务，无法核实完整业务活动；请先查看账户或任务。")}
                </p>
              )}
            </>
          )}
          {error && (
            <p role="alert">
              <ErrorNotice error={error} namespace="host" />
            </p>
          )}
        </ServiceActionDialog>
      )}
    </section>
  );
}
