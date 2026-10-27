import type { CommandHandler, CommandCall } from "../bridge/commands";
import { useEffect, useState } from "react";
import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
import type { NodeStatus, Snapshot } from "../bridge/client";
import {
  binding,
  kindLabels,
  location,
  matches,
  runtimeStatus,
  t,
  type Service,
  type ServiceKind,
} from "./service-state";
import { RemoteMachine, loadMachineProfiles, type MachineProfile } from "./RemoteMachine";
import { NodeIdentity } from "./NodeIdentity";
import { ServiceActionDialog } from "./ServiceActionDialog";
import { FirewallPreview } from "./FirewallPreview";
import { NodeResources } from "./NodeResources";
import { ManagedService, type ServiceAction } from "./ManagedService";
type Action = ServiceAction;
type Pending = { node: string; service: string; revision: string; action: Action };
const actionLabels: Record<Action, string> = {
  stop: "停止服务",
  restart: "重启服务",
  use: "切换运行位置",
  update: "更新服务程序",
};
// Trading has per-account placement, so the single-location summary excludes it.
const kinds: ServiceKind[] = ["market", "data", "research"];
export function NodeServices({
  snapshot,
  busy,
  trade,
}: {
  snapshot: Snapshot | null;
  busy: boolean;
  trade: CommandHandler;
}) {
  const [view, setView] = useState<"overview" | "machines">("overview");
  const [selected, setSelected] = useState("local");
  const [wizard, setWizard] = useState<MachineProfile | "new" | null>(null);
  // The machine whose identity is being replaced.
  const [identity, setIdentity] = useState<MachineProfile | null>(null);
  const [error, setError] = useState<DisplayError>("");
  const [saved, setSaved] = useState<MachineProfile[]>([]);
  const [pending, setPending] = useState<Pending | null>(null);
  const [deploying, setDeploying] = useState(false);
  const [deployment, setDeployment] = useState({
    kind: "research",
    service: "",
    port: "",
  });
  const [deployed, setDeployed] = useState<{ node: string; service: string } | null>(null);
  const nodes = snapshot?.nodes ?? [];
  const node = nodes.find(n => n.id === selected);
  const disabled = busy;
  async function run(...args: CommandCall) {
    setError("");
    try {
      await trade(...args);
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
    setIdentity(null);
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
      if (kind === "market") success = await run("market.attach", params);
      else if (kind === "data") success = await run("node.data.attach", params);
      else success = await run("node.research.attach", params);
    } else if (pending.action === "update")
      success = await run("node.update", { ...params, revision: pending.revision });
    else success = await run("node.action", { ...params, action: pending.action });
    if (success) setPending(null);
  }
  async function deploy() {
    if (!node || node.state !== "online" || node.health?.maintenance) return;
    if (
      await run("node.deploy", {
        id: node.id,
        ...deployment,
      })
    ) {
      setDeployed({ node: node.id, service: deployment.service });
      setDeploying(false);
    }
  }
  function startDeployment(targetId: string) {
    const target = nodes.find(item => item.id === targetId);
    const services = target?.health?.services ?? [];
    let suffix = 1;
    while (services.some(s => s.id === `work-${suffix}`)) suffix++;
    let port = 7443;
    while (port === target?.port || services.some(s => s.port === port)) port++;
    setDeployment({
      kind: "research",
      service: `work-${suffix}`,
      port: targetId === "local" ? "0" : String(port),
    });
    setDeploying(true);
    setDeployed(null);
  }
  const pendingActive = pendingService ? binding(snapshot, pendingService.kind) : null;
  const watched =
    pendingNode && pendingService && matches(pendingActive, pendingNode, pendingService);
  const tasks =
    watched && (pendingService?.kind === "research" || pendingService?.kind === "data")
      ? ((pendingService?.kind === "data"
          ? snapshot?.data
          : snapshot?.research_service
        )?.tasks.filter(task =>
          ["queued", "running", "cancel_requested", "publishing"].includes(task.state),
        ) ?? [])
      : [];
  return (
    <section className="deployment-center" aria-label={t("运行与部署")}>
      {!wizard && !identity && (
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
      {identity ? (
        <NodeIdentity
          key={identity.id}
          profile={identity}
          busy={disabled}
          run={run}
          onCancel={() => setIdentity(null)}
          onReplaced={() => manage(identity.id)}
        />
      ) : wizard ? (
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
                "本机 Terminal 使用当前系统账户运行服务管理器，无需 SSH。交易服务按账户绑定的节点和实例启动；默认在本机创建。",
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
                  <button
                    disabled={disabled}
                    onClick={() => {
                      setError("");
                      setIdentity(profile);
                    }}
                  >
                    {t("更换身份")}
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
                {saved
                  .filter(profile => profile.id === node.id)
                  .map(profile => (
                    <button
                      key={profile.id}
                      disabled={disabled}
                      onClick={() => {
                        setError("");
                        setIdentity(profile);
                      }}
                    >
                      {t("更换身份")}
                    </button>
                  ))}
              </div>
              {node.state !== "online" && (
                <p role="alert">{t("节点失联，以下是最后确认的状态。不会自动切换或重发命令。")}</p>
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
                        deployment.kind === "live"
                          ? "每个交易实例拥有独立进程和账本；部署后在交易页面绑定账户，不会自动登录或授权。"
                          : "数据服务执行取数并保管数据；研究服务执行回测和因子计算，需显式选择已部署的数据服务。",
                      )}
                    </p>
                    {node.id === "local" && deployment.kind === "research" && (
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
                        <option value="research">{t("研究服务")}</option>
                        <option value="data">{t("数据服务")}</option>
                        <option value="live">{t("CTP 交易")}</option>
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
                <NodeResources node={node} />
                {(node.health?.services ?? []).map(service => (
                  <ManagedService
                    key={service.id}
                    snapshot={snapshot}
                    node={node}
                    service={service}
                    disabled={disabled}
                    run={run}
                    propose={propose}
                  />
                ))}
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
