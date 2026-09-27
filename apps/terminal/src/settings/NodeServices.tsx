import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
import { translate, localizeText, type MessageValues, getLocale } from "../i18n";
const t = (key: string, values?: MessageValues) => translate("host", key, values);
import { useEffect, useState } from "react";
import { exportLinuxInitializer, type TerminalCommand, type Snapshot } from "../bridge/client";
const storageKey = "asterion.ssh-node-profiles.v1";
type Profile = {
    id: string;
    host: string;
    ssh_port: string;
    username: string;
    known_hosts: string;
    agent_port: string;
};
const empty: Profile = { id: "", host: "", ssh_port: "22", username: "asterion", known_hosts: "", agent_port: "7442" };
function load() {
    try {
        const value: unknown = JSON.parse(localStorage.getItem(storageKey) ?? "[]");
        if (!Array.isArray(value) || !value.every(p => p && Object.keys(p).length === Object.keys(empty).length && Object.keys(empty).every(k => typeof p[k] === "string")) || new Set(value.map(p => p.id)).size !== value.length)
            throw new Error("invalid configuration");
        return { profiles: value as Profile[], error: "" };
    }
    catch {
        return { profiles: [], error: t("节点配置无法读取，原始内容未改写。请修复配置后重新打开设置。") };
    }
}
export function NodeServices({ snapshot, busy, trade }: {
    snapshot: Snapshot | null;
    busy: boolean;
    trade: (method: TerminalCommand, params?: Record<string, unknown>) => Promise<void>;
}) {
    const [scope, setScope] = useState<"local" | "remote">("local");
    const [initial] = useState(load);
    const [profiles, setProfiles] = useState(initial.profiles);
    const [profile, setProfile] = useState<Profile>({ ...empty });
    const [privateKey, setPrivateKey] = useState("");
    const [keySource, setKeySource] = useState<"managed" | "provided">("managed");
    const managedKey = snapshot?.ssh_key?.id === profile.id ? snapshot.ssh_key : null;
    const keyReady = keySource === "managed" ? !!managedKey : !!privateKey.trim();
    const [copiedKey, setCopiedKey] = useState("");
    async function copyPublicKey() {
        if (!managedKey)
            return;
        try {
            await navigator.clipboard.writeText(managedKey.public_key);
            setCopiedKey(managedKey.id);
            setError("");
        }
        catch {
            setError(t("无法访问剪贴板，请手动复制上方公钥"));
        }
    }
    const [firewallPort, setFirewallPort] = useState("");
    const plan = snapshot?.firewall_plan;
    const agentProgram = snapshot?.agent_program;
    const [initializerStatus, setInitializerStatus] = useState("");
    async function exportInitializer() {
        try {
            if (await exportLinuxInitializer())
                setInitializerStatus(t("初始化脚本已导出，请复制到目标 Linux 并由管理员执行"));
        }
        catch (reason) {
            setError(asDisplayError(reason));
        }
    }
    const [error, setError] = useState<DisplayError>(initial.error);
    const [deployment, setDeployment] = useState({ service: "", port: "", kind:"paper" });
    const [target, setTarget] = useState("");
    async function run(method: TerminalCommand, params: Record<string, string>) {
        setError("");
        try {
            await trade(method, params);
        }
        catch (reason) {
            setError(asDisplayError(reason));
        }
    }
    useEffect(() => { void run("node.local", {}); }, []);
    return <section aria-label={t("部署与服务")}>
    <h2>{t("部署与服务")}</h2>
    <details><summary>{t("部署说明")}</summary><p>{t("本机和远程服务统一由 Node Agent 管理，关闭 Terminal 后继续运行。其他机器通过 SSH 安装 Node Agent，完成后通过安全管理连接部署和维护交易服务。节点与服务心跳由 C++ 后台维持，界面关闭设置页后仍持续监控。")}</p></details>
    <div role="group" aria-label={t("部署位置")}><button aria-pressed={scope === "local"} onClick={() => setScope("local")}>{t("本机部署")}</button><button aria-pressed={scope === "remote"} onClick={() => setScope("remote")}>{t("远程 Linux")}</button></div>
    {scope === "local" ? <section aria-label={t("本机部署说明")}><h3>{t("本机部署")}</h3><p>{t("支持 Windows、macOS 和 Linux。使用当前系统账户自动管理 Agent，无需 SSH、密钥或机器初始化脚本。在期货工作台创建模拟会话时，交易服务由本机 Agent 按需启动。")}</p><button disabled={busy} onClick={() => void run("node.local", {})}>{t("检查本机 Agent")}</button></section> : <p>{t("远程仅支持 Linux。请管理员先执行机器初始化脚本，再通过 SSH 安装 Agent；后续通过安全管理连接部署和维护服务。")}</p>}
    {scope === "local" && <details><summary>{t("Agent 程序")}</summary>
      <button disabled={busy} onClick={() => void run("node.agent.inspect", {})}>{t("检查程序更新")}</button>
      {agentProgram && <><p role="status">{({
        isolated:t("开发环境不管理系统安装"),
        not_installed:t("尚未安装本机 Agent"),
        current:t("已安装程序与安装包一致"),
        update_available:t("安装包中有不同版本"),
        recovery_required:t("存在未完成的更新，请先恢复"),
      })[agentProgram.state]}</p>
      {(agentProgram.state === "update_available" || agentProgram.state === "recovery_required") && <><p>{t("先停止所有业务服务。升级保留配置与数据，完成后手动启动服务。")}</p><button disabled={busy || !agentProgram.expected_digest || !!snapshot?.nodes?.find(n => n.id === "local")?.health?.services.some(s => s.desired_running || s.pid || s.active_workers)} onClick={() => void run("node.agent.upgrade", {expected_digest:agentProgram.expected_digest})}>{agentProgram.state === "recovery_required" ? t("继续恢复") : t("升级 Agent")}</button></>}
      {agentProgram.bundled_digest && <details><summary>{t("程序摘要")}</summary>
        <p>{t("已安装")}: <code>{agentProgram.installed_digest || "—"}</code></p>
        <p>{t("安装包")}: <code>{agentProgram.bundled_digest}</code></p>
      </details>}</>}
    </details>}
    {error && <p role="alert" className="alert"><ErrorNotice error={error} namespace="host"/></p>}
    <table className="data-table" aria-label={t("节点服务状态")}><thead><tr><th>{t("机器 / 服务")}</th><th>{t("状态")}</th><th>{t("最近心跳")}</th><th>{t("操作")}</th></tr></thead><tbody>
      {snapshot?.nodes?.filter(node => scope === "local" ? node.id === "local" : node.id !== "local").flatMap(node => [
            <tr key={node.id}><td>{node.id === "local" ? t("本机") : `${node.id} · ${node.host}:${node.port}`}</td><td>{node.state === "online" ? (node.health?.maintenance ? t("维护中") : t("节点在线")) : t("节点失联 · 服务状态未知")}</td><td>{new Date(node.last_heartbeat_ms).toLocaleTimeString(getLocale(), { hour12: false })} · {node.latency_ms} ms</td><td><button disabled={busy || node.id === "local"} onClick={() => void run("node.disconnect", { id: node.id })}>{t("移除监控")}</button><details><summary>{t("详情")}</summary>{node.health?.os} / {node.health?.arch} · {node.health?.version}<p>{node.error}</p></details></td></tr>,
            ...(node.health?.services ?? []).map(service => <tr key={`${node.id}/${service.id}`}><td>↳ {service.id}{service.port ? ` · :${service.port}` : ""}</td><td>{node.state !== "online" ? t("未知（最后确认）") : ({ running: t("进程运行"), stopped: t("已停止"), restarting: t("正在重启"), failed: t("启动失败") }[service.state] ?? service.state)}<details><summary>{t("详情")}</summary>PID {service.pid}{t("· 自动重启")}{service.restarts}/3<p>{service.error}</p></details></td><td>{service.last_heartbeat_ms ? new Date(service.last_heartbeat_ms).toLocaleTimeString(getLocale(), { hour12: false }) : "—"} · {({ ready: t("业务就绪"), awaiting_input: t("等待初始化"), degraded: t("需要恢复"), unresponsive: t("业务无响应"), starting: t("启动中"), offline: t("离线") }[service.health] ?? service.health)}</td><td><button disabled={busy || node.health?.maintenance || node.state !== "online" || service.state === "running"} onClick={() => void run("node.action", { id: node.id, service: service.id, action: "start" })}>{t("启动")}</button> <button disabled={busy || node.health?.maintenance || node.state !== "online" || !service.desired_running} onClick={() => void run("node.action", { id: node.id, service: service.id, action: "stop" })}>{t("停止")}</button> <button disabled={busy || node.health?.maintenance || node.state !== "online" || service.state !== "running"} onClick={() => void run("node.action", { id: node.id, service: service.id, action: "restart" })}>{t("重启")}</button> {<button disabled={busy || node.health?.maintenance || node.state !== "online" || service.state !== "running" || (service.kind==="paper" && !!snapshot.connection)} onClick={() => void run(service.kind==="strategy"?"strategy.attach":service.kind==="market"?"market.attach":service.kind==="research"?"research.attach":"node.attach", { id: node.id, service: service.id })}>{service.kind==="strategy"?t("查看策略"):service.kind==="market"?t("连接行情"):service.kind==="research"?t("连接研究"):t("连接交易")}</button>}<details><summary>{t("程序更新")}</summary><p>{t("先停止服务（会中断连接与任务），再更新程序。保留配置与数据，更新后需手动启动。")}</p><button disabled={busy || node.health?.maintenance || node.state !== "online" || service.state !== "stopped" || service.desired_running || !service.revision} onClick={() => void run("node.update", {id:node.id,service:service.id,revision:service.revision})}>{t("更新已停止的服务")}</button></details>{node.id !== "local" && <details><summary>{t("端口访问")}</summary><button disabled={busy || node.health?.maintenance || node.state !== "online" || node.id === "local"} onClick={() => void run("node.service_firewall", { id: node.id, service: service.id, action: "allow", token: "" })}>{t("检查端口放行")}</button><button disabled={busy || node.health?.maintenance || node.state !== "online" || node.id === "local"} onClick={() => void run("node.service_firewall", { id: node.id, service: service.id, action: "remove", token: "" })}>{t("检查规则撤销")}</button></details>}</td></tr>),
        ])}
    </tbody></table>
    {scope === "remote" && <>
    <section aria-label={t("准备远程 Linux")}><h3>{t("准备远程机器")}</h3><p>{t("安装包已内置 Linux x86_64 服务程序和初始化脚本，无需另外下载或选择程序文件。")}</p><button disabled={busy} onClick={() => void exportInitializer()}>{t("导出 Linux 初始化脚本")}</button>{initializerStatus && <p role="status">{localizeText("host", initializerStatus) ?? initializerStatus}</p>}<p>{t("复制脚本到目标机器，管理员执行")}<code>sudo python3 -I initialize-linux.py</code>{t("，按提示粘贴下方生成的公钥。完成后返回 Terminal 安装 Agent。")}</p></section>
    <details><summary>{t("通过 SSH 添加机器")}</summary>
      <p>{t("目标机器需启用 SSH。请选择经可信渠道核验的 known_hosts 文件；未知或变更的主机密钥会拒绝连接。请先由管理员在目标 Linux 执行独立初始化脚本，使用 asterion 账户。推荐在本机生成密钥，将公钥交给管理员初始化远程机器；私钥保留在本机，后续自动用于 SSH 登录。也可以临时使用已有未加密私钥。")}</p>
      <form onSubmit={event => { event.preventDefault(); try {
            const next = [...profiles.filter(p => p.id !== profile.id), profile];
            localStorage.setItem(storageKey, JSON.stringify(next));
            setProfiles(next);
            setError("");
        }
        catch {
            setError(t("无法保存节点配置"));
        } }}>
        <fieldset disabled={busy || !!initial.error}>
          <label className="terminal-setting">{t("机器配置")}<select aria-label={t("机器配置")} value={profiles.some(p => p.id === profile.id) ? profile.id : ""} onChange={e => { setPrivateKey(""); setProfile({ ...(profiles.find(p => p.id === e.target.value) ?? empty) }); }}><option value="">{t("新建机器")}</option>{profiles.map(p => <option key={p.id}>{p.id}</option>)}</select></label>
          <div className="futures-fields">{([["id", t("机器名称")], ["host", t("SSH 地址")], ["ssh_port", t("SSH 端口")], ["username", t("SSH 用户")], ["known_hosts", t("已核验 known_hosts 文件")], ["agent_port", t("Agent 管理端口")]] as const).map(([field, label]) => <label key={field}>{label}<input aria-label={label} required value={profile[field]} onChange={e => setProfile({ ...profile, [field]: e.target.value })}/></label>)}
          <label>{t("SSH 密钥来源")}<select aria-label={t("SSH 密钥来源")} value={keySource} onChange={e => { setPrivateKey(""); setKeySource(e.target.value as "managed" | "provided"); }}><option value="managed">{t("本机生成（推荐）")}</option><option value="provided">{t("使用已有私钥")}</option></select></label>
          {keySource === "managed" ? <section aria-label={t("本机 SSH 密钥")}><button type="button" disabled={!profile.id.trim()} onClick={() => void run("node.key.prepare", { id: profile.id })}>{t("生成或查看本机公钥")}</button><p>{t("先填写机器名称。私钥仅保存在本机受当前账户权限保护的密钥目录，不进入机器配置；相同机器名称复用已有密钥。")}</p>{managedKey && <><label>{t("初始化公钥")}<textarea aria-label={t("初始化公钥")} readOnly rows={3} value={managedKey.public_key}/></label><button type="button" onClick={() => void copyPublicKey()}>{t("复制公钥")}</button>{copiedKey === managedKey.id && <p role="status">{t("公钥已复制")}</p>}<p>{t("管理员在目标 Linux 运行初始化脚本并粘贴此公钥；确认主机指纹后，返回这里安装并连接。")}</p></>}</section> : <label>{t("SSH 私钥")}<textarea aria-label={t("SSH 私钥")} autoComplete="off" spellCheck={false} rows={5} value={privateKey} onChange={e => setPrivateKey(e.target.value)} placeholder={t("粘贴 SSH 私钥内容")}/></label>}</div>
          <details><summary>{t("安装前检查防火墙")}</summary>
            <p>{t("检查目标机器实际看到的 Terminal 来源 IP，并预览单个 TCP 端口的规则。首次安装检查 Agent 管理端口；已有交易服务也可在上方服务列表检查。")}</p>
            <label>{t("需要检查的端口")}<input aria-label={t("需要检查的端口")} value={firewallPort} placeholder={profile.agent_port} onChange={e => setFirewallPort(e.target.value)}/></label>
            <button type="button" disabled={!keyReady} onClick={() => void run("node.firewall.inspect", { ...profile, key_source: keySource, private_key: privateKey, firewall_port: firewallPort || profile.agent_port, firewall_action: "allow" })}>{t("检查并预览放行规则")}</button>
            <button type="button" disabled={!keyReady} onClick={() => void run("node.firewall.inspect", { ...profile, key_source: keySource, private_key: privateKey, firewall_port: firewallPort || profile.agent_port, firewall_action: "remove" })}>{t("检查已管理规则的撤销")}</button>
          </details>
          <div className="source-actions"><button type="submit">{t("保存机器配置")}</button><button type="button" disabled={!keyReady || !profiles.some(p => p.id === profile.id)} onClick={() => { const key = privateKey; setPrivateKey(""); void run("node.bootstrap", { ...profile, key_source: keySource, private_key: key }); }}>{t("通过 SSH 安装并连接")}</button><button type="button" disabled={!profiles.some(p => p.id === profile.id)} onClick={() => void run("node.connect", { id: profile.id })}>{t("连接已安装节点")}</button></div>
          {busy && <p role="status">{t("正在处理机器请求，请等待身份校验、安装与心跳验证完成…")}</p>}
        </fieldset>
      </form>
    </details>
    {plan && (plan.transport === "agent" || plan.id === profile.id) && <section aria-label={t("防火墙规则预览")}>
      <h3>{plan.action === "remove" ? t("撤销规则") : t("放行规则")} · {plan.id}</h3>
      <p>{t("目标")}{plan.host} · TCP {plan.port}{t("· 允许来源")}{plan.source}{t("（单台主机）")}</p>
      <p>{plan.backend} · {({ active: t("防火墙已启用"), read_only: t("防火墙已启用，专用账户仅有查询权限，请管理员放行此来源和端口"), permission_required: t("需要管理员或免交互 sudo 权限"), disabled: t("防火墙未启用，保持现状，请管理员确认网络策略"), unsupported: t("当前防火墙尚不支持自动配置，请管理员手动放行此来源和端口"), manual: t("此平台需手动配置来源 IP 与端口规则"), unknown: t("无法确定防火墙状态"), applied: t("规则已执行"), removed: t("本系统记录的规则已撤销") }[plan.state] ?? plan.state)}</p>
      {plan.verification === "tls_reachable" && <p role="status">{t("已从 Terminal 验证 TCP/mTLS 可达。")}</p>}
      {plan.verification === "pending_install" && <p role="status">{t("规则已执行；请安装并连接 Agent 后确认心跳。")}</p>}
      {plan.verification === "unreachable" && <p role="alert">{t("规则已执行，但 TCP/mTLS 仍不可达。请检查服务监听、云安全组、路由器或其他网络策略。")}</p>}
      {!plan.can_apply && plan.state === "active" && <p>{t("没有可执行的规则变更，请检查本系统是否拥有该规则。")}</p>}
      <details><summary>{t("规则详情")}</summary><code>{plan.rule}</code><p>{t("确认仅对上述来源和端口操作；五分钟后或目标状态变化后需要重新检查。只管理 Asterion 自己记录的规则。")}</p></details>
      {plan.can_apply && <button disabled={busy || (plan.transport === "ssh" && !keyReady)} onClick={() => {
                        if (plan.transport === "agent")
                            void run("node.service_firewall", { id: plan.id, service: plan.service!, action: "apply", token: plan.token });
                        else {
                            const key = privateKey;
                            setPrivateKey("");
                            void run("node.firewall.apply", { token: plan.token, private_key: key });
                        }
                    }}>{plan.action === "remove" ? t("确认撤销上述规则") : t("确认放行上述来源和端口")}</button>}
    </section>}
    <details><summary>{t("部署服务")}</summary>
      <p>{t("自动选择并校验内置 Linux 程序。每个服务使用独立数据目录，停止服务后保留数据。")}</p>
      <form onSubmit={event => { event.preventDefault(); void run("node.deploy", { id: target, ...deployment }); }}><fieldset disabled={busy}>
        <label className="terminal-setting">{t("服务类型")}<select aria-label={t("服务类型")} value={deployment.kind} onChange={e=>setDeployment({...deployment,kind:e.target.value})}><option value="paper">{t("历史模拟交易")}</option><option value="market">{t("实时行情")}</option><option value="research">{t("研究服务")}</option></select></label><label className="terminal-setting">{t("部署目标")}<select aria-label={t("部署目标")} required value={target} onChange={e => setTarget(e.target.value)}><option value="">{t("选择在线节点")}</option>{snapshot?.nodes?.filter(n => n.state === "online" && !n.health?.maintenance && n.id !== "local").map(n => <option key={n.id} value={n.id}>{n.id} · {n.health?.os}/{n.health?.arch}</option>)}</select></label>
        <div className="futures-fields">{([["service", t("新服务名称")], ["port", t("服务端口")]] as const).map(([field, label]) => <label key={field}>{label}<input aria-label={label} required value={deployment[field]} onChange={e => setDeployment({ ...deployment, [field]: e.target.value })}/></label>)}</div>
        <div className="source-actions"><button type="submit" disabled={!target}>{t("上传并部署")}</button></div>
      </fieldset></form>
    </details>
    </>}
    <p>{t("Agent 独立检测进程与业务心跳；连续三次无响应会触发恢复。服务异常最多自动重启 3 次；网络失联不会触发重复部署或重复下单。")}</p>
  </section>;
}
