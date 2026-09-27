import { translate, type MessageValues } from "../contract";
const t = (key: string, values?: MessageValues) => translate("asterion.terminal.futures-market", key, values);
import { timestamp, type Dataset } from "@asterion/desktop-bridge/client";
export function MarketPanel({ data, openData }: {
    data: Dataset | null;
    openData: () => void;
}) {
    const prices = data?.ticks.map(t => Number(t.price)) ?? [];
    const low = Math.min(...prices), high = Math.max(...prices);
    const span = high - low || 1;
    const points = prices.map((price, index) => `${20 + index / Math.max(1, prices.length - 1) * 960},${220 - (price - low) / span * 190}`).join(" ");
    return <section className="futures-market" aria-label={t("期货行情")}>
    <div className="panel-heading"><h2>{t("历史逐笔行情")}</h2><span className="panel-spacer"/><button onClick={openData}>{t("选择本地数据")}</button></div>
    {!data || !data.count ? <div className="dashboard-empty"><strong>{data ? t("该文件没有成交记录") : t("尚未选择期货数据")}</strong><p>{t("连接尚未接入。选择并校验本地逐笔 CSV 后，可在此查看历史成交。")}</p><button onClick={openData}>{t("打开数据工作区")}</button></div> : <>
      <div className="quote-strip"><b>{data.venue} · {data.symbol}</b><span>{t("最后成交")}<strong>{data.last_price}</strong></span><span>{t("总成交量 {quantity} 手", { quantity: data.quantity })}</span><span>{t("数据截至")}{" "}{timestamp(data.last_timestamp_ns)}</span><span className="subtle">{t("历史数据 · 非实时")}</span></div>
      <div className="futures-chart"><svg viewBox="0 0 1000 250" role="img" aria-label={t("{p0}最近{p1}笔历史成交价格", { p0: data.symbol, p1: prices.length })} preserveAspectRatio="none"><path d="M20 30H980 M20 125H980 M20 220H980" stroke="var(--grid)" fill="none"/>{prices.length === 1 ? <circle cx="20" cy="220" r="3" fill="var(--accent)"/> : <polyline points={points} fill="none" stroke="var(--accent)" strokeWidth="1.5"/>}</svg><span className="dashboard-caption">{t("最近 {count} 笔 · 按成交顺序绘制 · 北京时间", { count: prices.length })}</span></div>
      <div className="futures-table"><table className="data-table"><thead><tr><th>{t("成交时间（北京时间）")}</th><th>{t("价格")}</th><th>{t("成交量（手）")}</th></tr></thead><tbody>{[...data.ticks].reverse().map((tick, index) => <tr key={index}><td title={tick.timestamp_ns}>{timestamp(tick.timestamp_ns)}</td><td className="numeric">{tick.price}</td><td className="numeric">{tick.quantity}</td></tr>)}</tbody></table></div>
    </>}
  </section>;
}
