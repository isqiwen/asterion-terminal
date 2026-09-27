import "./data.css";
import { CalendarPublications } from "./CalendarPublications";
import { type TerminalContext, getLocale } from "../contract";
import { ErrorNotice, asDisplayError, type DisplayError } from "../contract";
import { translate, type MessageValues } from "../contract";
const t = (key: string, values?: MessageValues) => translate("asterion.terminal.data-workbench", key, values);
import { useState, type FormEvent } from "react";
import { open } from "@tauri-apps/plugin-dialog";
import { nativeDesktop } from "@asterion/desktop-bridge/desktop";
import { timestamp, type CsvRequest, type TerminalCommand } from "@asterion/desktop-bridge/client";
export function DataPanel({snapshot, busy, inspect, navigate, trade}: TerminalContext) {
    const data = snapshot?.dataset ?? null;
    const research = snapshot?.research;
    const tasks = research?.tasks.filter(task=>task.kind==="data_import") ?? [];
    const openMarket = () => navigate("workspace.market", {marketMode:"history"});
    const [pendingId, setPendingId] = useState<string|null>(null);
    async function run(method: TerminalCommand, params: Record<string,unknown> = {}) {
        setError("");
        try { await trade(method,params); return true; }
        catch(reason) { setError(asDisplayError(reason)); return false; }
    }
    async function publish() {
        const id=pendingId??`data-${crypto.randomUUID()}`;
        setPendingId(id);
        if(await run("research.data.submit",{id})) { setPendingId(null); setMessage("发布任务已提交，可关闭窗口。"); }
    }
    const [form, setForm] = useState<CsvRequest>({ path: "", venue: "SHFE", symbol: "", product: "", delivery_month: "", currency: "CNY", price_increment: "", quantity_increment: "1", multiplier: "" });
    const [error, setError] = useState<DisplayError>("");
    const [message, setMessage] = useState("");
    function field(name: keyof CsvRequest, value: string) { setForm(previous => ({ ...previous, [name]: value })); setMessage(""); }
    async function selectFile() {
        try {
            const path = await open({ multiple: false, directory: false, filters: [{ name: "CSV", extensions: ["csv"] }] });
            if (typeof path === "string")
                field("path", path);
        }
        catch (reason) {
            setError(asDisplayError(reason));
        }
    }
    async function submit(event: FormEvent) {
        event.preventDefault();
        setError("");
        setMessage("");
        try {
            await inspect(form);
            setPendingId(null);
            setMessage("文件校验完成，可在市场工作区查看历史成交。");
        }
        catch (reason) {
            setError(asDisplayError(reason));
        }
    }
    return <section className="futures-data" aria-label={t("期货数据")}>
    <div className="panel-heading"><h2>{t("期货数据")}</h2><span className="panel-spacer"/><small>{t("CSV 预览与版本发布")}</small></div>
    <details className="data-import" open={!data?.persistent}><summary>{t("导入 CSV")}</summary>
    <form onSubmit={submit}>
      <fieldset disabled={busy}>
        <div className="futures-file"><label>{t("CSV 文件")}<input aria-label={t("CSV 文件路径")} value={form.path} onChange={e => field("path", e.target.value)} placeholder={t("选择本机历史逐笔数据文件")} required/></label>
          {nativeDesktop && <button type="button" onClick={() => void selectFile()}>{t("选择文件")}</button>}</div>
        <div className="futures-fields">
          <label>{t("交易所")}<select aria-label={t("交易所")} value={form.venue} onChange={e => field("venue", e.target.value)}>{["SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX"].map(venue => <option key={venue}>{venue}</option>)}</select></label>
          <label>{t("品种代码")}<input aria-label={t("品种代码")} placeholder={t("如 rb")} value={form.product} onChange={e => field("product", e.target.value)} required/></label>
          <label>{t("实际合约")}<input aria-label={t("实际合约")} placeholder={t("如 rb2610")} value={form.symbol} onChange={e => field("symbol", e.target.value)} required/></label>
          <label>{t("交割月份")}<input aria-label={t("交割月份")} type="month" value={form.delivery_month} onChange={e => field("delivery_month", e.target.value)} required/></label>
          <label>{t("价格步长")}<input aria-label={t("价格步长")} inputMode="decimal" value={form.price_increment} onChange={e => field("price_increment", e.target.value)} required/></label>
          <label>{t("每手乘数")}<input aria-label={t("每手乘数")} inputMode="numeric" value={form.multiplier} onChange={e => field("multiplier", e.target.value)} required/></label>
          <label>{t("手数步长")}<input aria-label={t("手数步长")} inputMode="numeric" value={form.quantity_increment} onChange={e => field("quantity_increment", e.target.value)} required/></label>
          <label>{t("计价币种")}<input aria-label={t("计价币种")} value={form.currency} onChange={e => field("currency", e.target.value)} required/></label>
        </div>
        <div className="source-actions"><button className="primary" disabled={busy} type="submit">{busy ? t("正在校验…") : t("校验并预览")}</button></div>
      </fieldset>
    </form>
    <p className="dashboard-caption">{t("合约规格由你提供。预览仅保留在当前会话；发布后可重复用于研究。")}</p>
    <details className="futures-help"><summary>{t("文件格式与校验规则")}</summary><p>{t("首行：")}<code>timestamp_ns,price,quantity</code>{t("。时间为 UTC 纳秒，按时间升序；价格和数量最多八位小数，数量单位为手。仅接受实际月份合约，不接受主力或连续合约。")}</p><p>{t("最多 32 MiB / 250000 笔；完整校验后展示最近 240 笔。失败时保留上次成功预览。")}</p></details>
    </details>
    {error && <p className="alert" role="alert"><ErrorNotice error={error} namespace="asterion.terminal.data-workbench"/></p>}
    {message && <p role="status" className="notice">{t(message)}</p>}
    {data && <div className="dataset-summary"><div><dt>{t(data.persistent?"已发布数据":"当前预览")}</dt><dd>{data.venue} · {data.symbol}</dd></div><div><dt>{t("记录数")}</dt><dd>{data.count.toLocaleString(getLocale())} {t("笔")}</dd></div><div><dt>{t("数据截至（北京时间）")}</dt><dd>{timestamp(data.last_timestamp_ns)}</dd></div></div>}
    {data && <div className="source-actions"><button onClick={openMarket}>{t("查看行情")}</button>{!data.persistent&&<button disabled={busy||!research?.online||!data.publication_ready} onClick={()=>void publish()}>{t(pendingId?"确认发布状态":"发布数据版本")}</button>}{data.persistent&&<button onClick={()=>navigate("workspace.research")}>{t("开始研究")}</button>}{!data.persistent&&<small>{t("发布上限：10000 笔 / 4 MiB")}</small>}</div>}
    {data?.publication_id&&<details className="futures-help"><summary>{t("版本详情")}</summary><p>{t("发布标识")} <code>{data.publication_id}</code></p><p>{t("内容版本")} <code>{data.revision}</code></p></details>}
    <section className="data-publications" aria-label={t("数据发布")}>
      <div className="panel-heading"><h3>{t("数据发布")}</h3><span className="panel-spacer"/><small>{research?.remote?`${research.host} · ${research.service}`:t("本机")}</small>{!research?.online&&<button disabled={busy} onClick={()=>void run("research.local")}>{t("连接研究服务")}</button>}</div>
      {!tasks.length?<p className="dashboard-caption">{t("暂无发布任务")}</p>:<div className="publication-list">{[...tasks].reverse().map(task=><article key={task.id} className="publication-row">
        <div className="publication-name"><strong>{task.source_name}</strong><small>{task.instrument}</small><time aria-label={t("提交时间")} dateTime={new Date(task.submitted_at_ms).toISOString()}>{new Date(task.submitted_at_ms).toLocaleString(getLocale())}</time></div><span>{t(({queued:"排队中",running:"发布中",cancel_requested:"正在取消",succeeded:"已发布",failed:"失败",cancelled:"已取消",interrupted:"已中断"})[task.state])}{!research?.online&&` · ${t("最后确认状态")}`}</span>
        <div>{task.state==="succeeded"&&<button disabled={busy||!research?.online} onClick={()=>void run("research.data.use",{id:task.id}).then(ok=>{if(ok){setPendingId(null);setMessage("已载入发布版本，可查看行情或开始研究。");}})}>{t("使用此版本")}</button>}{["queued","running"].includes(task.state)&&<button disabled={busy||!research?.online} onClick={()=>void run("research.action",{id:task.id,action:"cancel"})}>{t("取消")}</button>}{["failed","cancelled","interrupted"].includes(task.state)&&<button disabled={busy||!research?.online} onClick={()=>void run("research.action",{id:task.id,action:"retry"})}>{t("重新运行")}</button>}</div>
        <details><summary>{t("详情")}</summary><code>{task.id}</code><p>{task.completed} / {task.total} {t("字节")}</p><p>{t("最近更新")} · {new Date(task.updated_at_ms).toLocaleString(getLocale())}</p>{task.error&&<p>{task.error}</p>}</details>
      </article>)}</div>}
    </section>
    <CalendarPublications snapshot={snapshot} busy={busy} trade={trade}/>
  </section>;
}
