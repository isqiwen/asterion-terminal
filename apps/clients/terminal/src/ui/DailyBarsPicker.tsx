import type { CommandCall } from "../bridge/commands";
import { useState } from "react";
import { translate, type MessageValues } from "../i18n";
import type { dailyTableOf } from "./useDailyTable";
import { DailyTableGaps } from "./DailyTableGaps";
const t = (key: string, values?: MessageValues) => translate("host", key, values);

// What is read of the daily table: one contract's own days, or a product's
// months as its dominant series. Both are named as history names them.
const kinds = {
  contract: {
    name: "日线合约",
    command: "data.dataset.daily",
    parameter: "contract_id",
    pattern: /^[A-Z]+\/[a-z]+\/\d{4}-\d{2}$/,
    field: "合约",
    placeholder: "例如 SHFE/rb/2026-10",
    empty:
      "全市场日线表里还没有数据。请先在「数据 → 全市场数据」更新日线，再从这里选择合约的日线。",
    about:
      "直接读取全市场日线表里一个合约的日线：有成交的交易日是一根 K 线，结算价取同一行。不需要另外下载。",
    submit: "使用这个合约的日线",
  },
  product: {
    name: "日线主力连续",
    command: "data.dataset.product",
    parameter: "product",
    pattern: /^[A-Z]+\/[a-z]+$/,
    field: "品种",
    placeholder: "例如 SHFE/rb",
    empty:
      "全市场日线表里还没有数据。请先在「数据 → 全市场数据」更新日线，再从这里按品种选择日线的主力连续。",
    about:
      "把一个品种在全市场日线表里的各月份合约作为主力连续回测：范围内有成交的月份都参加，每个交易日的主力是前一交易日持仓量最大的月份，只向更远的月份切换。不需要另外下载。",
    submit: "使用这个品种的日线主力连续",
  },
} as const;

// Chooses daily bars of the market-wide daily table: nothing is downloaded for
// them, and what the table does not hold of the range is refused by name. Of
// a product, the data service finds the months that traded in the range and
// works out which is dominant each day; nothing here decides the schedule.
export function DailyBarsPicker({
  kind,
  daily,
  disabled,
  select,
}: {
  kind: keyof typeof kinds;
  daily: ReturnType<typeof dailyTableOf>;
  disabled: boolean;
  select: (...args: CommandCall<"data.dataset.daily" | "data.dataset.product">) => void;
}) {
  const words = kinds[kind];
  const [instrument, setInstrument] = useState("");
  const [begin, setBegin] = useState("");
  const [end, setEnd] = useState("");
  if (!daily) return <p role="status">{t(words.empty)}</p>;
  const named = words.pattern.test(instrument.trim());
  const exchange = named ? instrument.trim().split("/")[0] : undefined;
  const settled = daily.stored.find(row => row.exchange === exchange);
  return (
    <form
      aria-label={t(words.name)}
      onSubmit={event => {
        event.preventDefault();
        const range = { daily: daily.table, begin_day: begin, end_day: end };
        if (kind === "contract")
          select("data.dataset.daily", { ...range, contract_id: instrument.trim() });
        else select("data.dataset.product", { ...range, product: instrument.trim() });
      }}
    >
      <p className="subtle">{t(words.about)}</p>
      <fieldset disabled={disabled}>
        <div className="futures-fields">
          <label className="dataset-version-field">
            {t(words.field)}
            <input
              placeholder={t(words.placeholder)}
              value={instrument}
              onChange={event => setInstrument(event.target.value)}
            />
          </label>
          <label>
            {t("开始交易日")}
            <input type="date" value={begin} onChange={event => setBegin(event.target.value)} />
          </label>
          <label>
            {t("结束交易日")}
            <input
              type="date"
              min={begin || undefined}
              value={end}
              onChange={event => setEnd(event.target.value)}
            />
          </label>
        </div>
        {named && settled && (
          <p className="subtle">
            {t(
              "{exchange} 的日线已入库 {first} — {last}，按快照 {snapshot} 读取，之后入库的数据不影响这次选择。",
              {
                exchange: settled.exchange,
                first: settled.first_period,
                last: settled.last_period,
                snapshot: daily.table.snapshot.slice(0, 12),
              },
            )}
          </p>
        )}
        {named && settled && <DailyTableGaps stored={settled} />}
        {named && !settled && (
          <p role="status">
            {t("全市场日线表里还没有 {exchange} 的数据。", { exchange: exchange ?? "" })}
          </p>
        )}
        <div className="source-actions">
          <button
            type="submit"
            className="primary"
            disabled={!named || !settled || !begin || !end || begin > end}
          >
            {t(words.submit)}
          </button>
          <span className="subtle">
            {t("起止交易日都要填写；范围内日线表缺的交易日会被拒绝并列出日期。")}{" "}
            {t("合约乘数取自合约表，最小变动价位按每个交易日的成交价推出。")}
          </span>
        </div>
      </fieldset>
    </form>
  );
}
