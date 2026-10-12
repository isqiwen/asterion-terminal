import { useState } from "react";
import { translate, type MessageValues } from "../i18n";
import type { HistoryDatasetRecord, Snapshot } from "../bridge/client";
import type { dailyTableOf } from "./useDailyTable";
const t = (key: string, values?: MessageValues) => translate("host", key, values);

// Mirrors the core's portfolio limit.
const maxMonths = 20;
const latest = (items: HistoryDatasetRecord[]) =>
  items.reduce<HistoryDatasetRecord | undefined>(
    (best, item) => (!best || item.end > best.end ? item : best),
    undefined,
  );
type Month = { contract: string; bars: HistoryDatasetRecord };
type Group = { key: string; product: string; interval: number; source: string; months: Month[] };
// Products whose months can form a dominant series: at least two months with
// bars of one source and interval. A month uses its download that ends last;
// what each month settled at and held comes from the market-wide daily table.
function groups(versions: HistoryDatasetRecord[]): Group[] {
  const result = new Map<string, Group>();
  const contracts = [...new Set(versions.map(item => item.contract_id))].sort();
  for (const contract of contracts) {
    const own = versions.filter(item => item.contract_id === contract);
    const product = contract.split("/").slice(0, 2).join("/");
    for (const interval of new Set(own.map(item => item.interval_minutes)))
      for (const source of new Set(own.map(item => item.source))) {
        const bars = latest(
          own.filter(item => item.interval_minutes === interval && item.source === source),
        );
        if (!bars) continue;
        const key = `${product}|${interval}|${source}`;
        const group = result.get(key) ?? { key, product, interval, source, months: [] };
        group.months.push({ contract, bars });
        result.set(key, group);
      }
  }
  return [...result.values()].filter(group => group.months.length >= 2);
}
// Chooses the month contracts of one product as its dominant series. The data
// service works out which month is dominant on each day from the previous
// day's open interest; nothing here decides the schedule.
export function DominantSeriesPicker({
  snapshot,
  versions,
  daily,
  disabled,
  select,
}: {
  snapshot: Snapshot | null;
  versions: HistoryDatasetRecord[];
  daily: ReturnType<typeof dailyTableOf>;
  disabled: boolean;
  select: (params: Record<string, unknown>) => void;
}) {
  const available = groups(versions);
  const [key, setKey] = useState("");
  const [begin, setBegin] = useState("");
  const [end, setEnd] = useState("");
  const [tick, setTick] = useState("");
  const [multiplier, setMultiplier] = useState("");
  if (!available.length)
    return <p className="subtle">{t("主力连续需要同一品种至少两个月份的 K 线。")}</p>;
  const group = available.find(item => item.key === key);
  // What the daily table holds of the product's exchange.
  const exchange = group?.product.split("/")[0];
  const settled = daily?.stored.find(row => row.exchange === exchange);
  // Months whose bars reach into the chosen trading days.
  const months = (group?.months ?? []).filter(
    month =>
      (!begin || month.bars.end.slice(0, 10) >= begin) &&
      (!end || month.bars.begin.slice(0, 10) <= end),
  );
  const choose = (next: string) => {
    setKey(next);
    const first = available.find(item => item.key === next)?.months[0];
    const listed = snapshot?.market?.catalog.contracts.find(
      contract => contract.contract_id === first?.contract,
    );
    // Never reuse another product's manually entered exchange units.
    setTick(listed?.price_tick ?? "");
    setMultiplier(listed ? String(listed.multiplier) : "");
  };
  return (
    <section className="dataset-composition" aria-label={t("主力连续")}>
      <p className="subtle">
        {t(
          "把一个品种的各月份合约作为主力连续回测：每个交易日的主力是前一交易日持仓量最大的月份，只向更远的月份切换。策略信号使用等比前复权价格，成交和盈亏使用真实合约的真实价格。",
        )}
      </p>
      <fieldset disabled={disabled}>
        <div className="futures-fields">
          <label className="dataset-version-field">
            {t("品种")}
            <select aria-label={t("品种")} value={key} onChange={e => choose(e.target.value)}>
              <option value="">{t("选择品种")}</option>
              {available.map(item => (
                <option key={item.key} value={item.key}>
                  {item.product} · {item.interval ? t("{n} 分钟", { n: item.interval }) : t("日线")}{" "}
                  · {item.source} · {t("{n} 个月份", { n: item.months.length })}
                </option>
              ))}
            </select>
          </label>
          <label>
            {t("开始交易日")}
            <input type="date" value={begin} onChange={e => setBegin(e.target.value)} />
          </label>
          <label>
            {t("结束交易日")}
            <input type="date" value={end} onChange={e => setEnd(e.target.value)} />
          </label>
          <label>
            {t("最小变动价位")}
            <input inputMode="decimal" value={tick} onChange={e => setTick(e.target.value)} />
          </label>
          <label>
            {t("合约乘数")}
            <input
              inputMode="decimal"
              value={multiplier}
              onChange={e => setMultiplier(e.target.value)}
            />
          </label>
        </div>
        {group && months.length > maxMonths && (
          <p role="status">
            {t("所选交易日内有 {n} 个月份，最多 {max} 个；请缩小交易日区间。", {
              n: months.length,
              max: maxMonths,
            })}
          </p>
        )}
        {group && months.length < 2 && (
          <p role="status">{t("所选交易日内不足两个月份，无法组成主力连续。")}</p>
        )}
        {group && settled && daily && (
          <p className="subtle">
            {t(
              "结算价、交易日和持仓量取自全市场日线表：{exchange} 已入库 {first} — {last}，按快照 {snapshot} 读取。",
              {
                exchange: settled.exchange,
                first: settled.first_period,
                last: settled.last_period,
                snapshot: daily.table.snapshot.slice(0, 12),
              },
            )}
          </p>
        )}
        {group && !settled && (
          <p role="status">
            {t(
              "全市场日线表里还没有 {exchange} 的数据，结算价无处可取。请先在「数据 → 全市场数据」更新日线，范围要覆盖这些 K 线的交易日。",
              { exchange: exchange ?? "" },
            )}
          </p>
        )}
        <div className="source-actions">
          <button
            type="button"
            className="primary"
            disabled={
              !group ||
              !settled ||
              months.length < 2 ||
              months.length > maxMonths ||
              !tick ||
              !multiplier
            }
            onClick={() =>
              daily &&
              select({
                source_dataset_ids: months.map(month => month.bars.id),
                daily: daily.table,
                begin_day: begin,
                end_day: end,
                price_increment: tick,
                multiplier,
              })
            }
          >
            {t("使用主力连续（{n} 个月份）", { n: months.length })}
          </button>
        </div>
      </fieldset>
    </section>
  );
}
