import type { QuoteRow } from "../../src/generated/asterion/market_pb";
import { rowKey, type MarketView } from "../../src/host/market";
import { contractName, translate } from "../contract";
import { quoteChange } from "./QuoteTable";
import { selectQuote } from "./selected-quote";
const t = (key: string) => translate("asterion.terminal.futures-market", key);

// Months of one product share the exchange and the letters of the symbol.
export const sameProduct = (a: QuoteRow, b: QuoteRow) =>
  a.instrument!.exchange === b.instrument!.exchange &&
  a.instrument!.symbol.replace(/\d/g, "") === b.instrument!.symbol.replace(/\d/g, "");
const tone = (row: QuoteRow) => {
  const amount = quoteChange(row)?.amount;
  return !amount ? undefined : amount > 0 ? "up" : "down";
};

// Under the chart: the day's figures, five levels of depth and the product's other months.
export function ContractQuote({ market, row }: { market: MarketView; row: QuoteRow }) {
  const q = row.quote;
  const months = market.board.rows
    .filter(other => sameProduct(other, row))
    .sort((a, b) => a.instrument!.symbol.localeCompare(b.instrument!.symbol));
  const levels = [4, 3, 2, 1, 0];
  return (
    <section className="contract-quote" aria-label={t("盘口")}>
      <table className="contract-depth" aria-label={t("买卖盘")}>
        <tbody>
          {levels.map(level => (
            <tr key={`ask${level}`}>
              <th>
                {t("卖")}
                {level + 1}
              </th>
              <td data-tone="up">{q?.asks[level]?.price?.text ?? "—"}</td>
              <td>{q?.asks[level]?.lots?.toString() ?? "—"}</td>
            </tr>
          ))}
          {[0, 1, 2, 3, 4].map(level => (
            <tr key={`bid${level}`} data-first-bid={level === 0 || undefined}>
              <th>
                {t("买")}
                {level + 1}
              </th>
              <td data-tone="down">{q?.bids[level]?.price?.text ?? "—"}</td>
              <td>{q?.bids[level]?.lots?.toString() ?? "—"}</td>
            </tr>
          ))}
        </tbody>
      </table>
      <dl className="contract-figures">
        {[
          ["开盘", q?.open?.text],
          ["最高", q?.high?.text],
          ["最低", q?.low?.text],
          ["昨结算", q?.previousSettlement?.text],
          ["涨停", q?.upperLimit?.text],
          ["跌停", q?.lowerLimit?.text],
          ["持仓量", q?.openInterest?.text],
          ["日增仓", q?.openInterestChange?.text],
          ["成交量", q?.volume.toString()],
          ["交易日", q?.tradingDay],
        ].map(([label, value]) => (
          <div key={label}>
            <dt>{t(label!)}</dt>
            <dd>{value || "—"}</dd>
          </div>
        ))}
      </dl>
      {months.length > 1 && (
        <details className="contract-months" open>
          <summary>{t("同品种其他月份")}</summary>
          {months.map(month => {
            const change = quoteChange(month);
            return (
              <button
                key={rowKey(month)}
                aria-pressed={rowKey(month) === rowKey(row)}
                onClick={() => selectQuote(rowKey(month))}
              >
                <span>{contractName(month.instrument!, market.board.catalog)}</span>
                <span data-tone={tone(month)}>{month.quote?.last?.text ?? "—"}</span>
                <span data-tone={tone(month)}>
                  {change ? `${change.percent > 0 ? "+" : ""}${change.percent.toFixed(2)}%` : "—"}
                </span>
              </button>
            );
          })}
        </details>
      )}
    </section>
  );
}
