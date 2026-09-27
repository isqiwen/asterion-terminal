import { useState } from "react";
import { ErrorNotice, asDisplayError, translate, getLocale, type DisplayError, type TerminalContext } from "../contract";
const t = (key: string) => translate("asterion.terminal.trading", key);

export function StrategyPanel({ snapshot, busy, trade }: Pick<TerminalContext, "snapshot" | "busy" | "trade">) {
    const [id] = useState(() => crypto.randomUUID());
    const [parameters, setParameters] = useState({ fast: "5", slow: "20", quantity: "1", calendar_task: "" });
    const [attempted, setAttempted] = useState(false);
    const [error, setError] = useState<DisplayError>("");
    const paper = snapshot?.paper, strategy = snapshot?.strategy;
    const grant = paper?.strategy;
    const active = !!grant?.active;
    const ready = !!paper && paper.storage_state === "ready" && snapshot?.connection?.transport === "local";
    const fresh = ready && !grant && paper.cursor === 0 && !paper.orders.length && !paper.positions.length;
    const retry = attempted && grant?.strategy_id === `strategy-${id}` && strategy?.phase === "uninitialized";
    const phases: Record<string, string> = { waiting: t("等待服务"), running: t("运行中"), completed: t("已完成"), blocked: t("已暂停"), uninitialized: t("等待配置"), manual: t("外部事件模式"), unknown: t("状态未知") };
    async function run() {
        setError(""); setAttempted(true);
        try { await trade("strategy.run", { id, ...parameters }); }
        catch (reason) { setError(asDisplayError(reason)); }
    }
    async function revoke() {
        setError("");
        try { await trade("strategy.revoke", { grant_id: grant!.grant_id }); }
        catch (reason) { setError(asDisplayError(reason)); }
    }
    return <section aria-label={t("策略运行")} className="strategy-panel">
        <div className="panel-heading"><h3>{t("策略运行")}</h3><span className="subtle">{t("历史模拟 · 双均线")}</span><span className="panel-spacer"/>{active && <button disabled={busy || paper?.storage_state !== "ready" || snapshot?.connection?.state !== "connected"} onClick={() => void revoke()}>{t("撤销策略授权")}</button>}</div>
        {strategy && <div className="paper-toolbar" role="status"><strong>{strategy.symbol ?? "—"}</strong><span>{strategy.state === "disconnected" ? t("连接中断") : phases[strategy.phase] ?? t("状态未知")}</span><span>{strategy.processed} / {strategy.total}</span><progress aria-label={t("策略进度")} max={strategy.total || 1} value={strategy.processed}/></div>}
        {active && <p className="dashboard-caption">{t("策略控制此账户。撤销授权后可手动操作，已有持仓保留。")}</p>}
        {(fresh || retry) && <form onSubmit={event => { event.preventDefault(); void run(); }}><fieldset disabled={busy}>
            <div className="futures-fields">{([ ["fast", "短周期"], ["slow", "长周期"], ["quantity", "目标手数"] ] as const).map(([key, label]) => <label key={key}>{t(label)}<input aria-label={t(label)} inputMode="numeric" value={parameters[key]} disabled={attempted && (active || strategy?.id === `strategy-${id}`)} onChange={event => setParameters({ ...parameters, [key]: event.target.value })} required/></label>)}</div>
            <label>{t("日程来源")}<select aria-label={t("日程来源")} value={parameters.calendar_task} disabled={attempted && (active || strategy?.id === `strategy-${id}` || !!paper?.replay)} onChange={event=>setParameters({...parameters,calendar_task:event.target.value})}><option value="">{t("无日程（不自动结算）")}</option>{snapshot?.research?.tasks.filter(task=>task.kind==="calendar_import"&&task.state==="succeeded").map(task=><option key={task.id} value={task.id}>{task.source_name} · {task.instrument} · {new Date(task.submitted_at_ms).toLocaleString(getLocale())}</option>)}</select></label>
            <div className="source-actions"><button type="submit" className="primary" disabled={!snapshot?.dataset}>{t(retry ? "重试同一配置" : "授权并运行")}</button><span className="subtle">{t("关闭窗口后继续运行")}</span></div>
        </fieldset></form>}
        {!fresh && !active && !retry && strategy?.phase !== "completed" && <p className="dashboard-caption">{t("选择历史数据并创建新的本机模拟账户后，可运行策略。")}</p>}
        {paper?.replay && <details className="futures-help"><summary>{t("结算日程")} · {paper.replay.settled_days} / {paper.replay.publication.calendar.days.length}</summary><p>{paper.replay.publication.source_name}</p><p>{t("发布标识")} <code>{paper.replay.publication.id}</code></p><p>{t("内容版本")} <code>{paper.replay.publication.calendar.revision}</code></p></details>}
        {error && <p role="alert" className="alert"><ErrorNotice error={error} namespace="asterion.terminal.trading"/></p>}
        <details className="futures-help"><summary>{t("策略详情")}</summary><p>{t("按成交笔数计算均线，仅做多或空仓；信号最早在下一笔撮合。结束时撤销未成交委托，不自动平仓。")}</p>{strategy && <><p>{strategy.id} · {strategy.fast} / {strategy.slow} · {strategy.quantity}</p><p>{strategy.error}</p></>}</details>
    </section>;
}
