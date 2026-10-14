import { translate, type MessageValues } from "../i18n";
import type { FuturesContract } from "../bridge/client";
const t = (key: string, values?: MessageValues) => translate("host", key, values);

export type PriceIncrementChange = { trading_day: string; value: string };
// A dataset as its units are shown: its contract, whose price increment is
// that of its first bar's day, and the days it changed after that.
export type DatasetUnits = {
  contract: FuturesContract;
  first_day: string;
  price_increment_changes: PriceIncrementChange[];
};
// The price increment of one contract, or of the months of one series, over
// the days read: the first one, then each change ("0.05、2019-12-10 起 0.02").
export function priceIncrements(datasets: DatasetUnits[]) {
  const days = new Map<string, string>();
  for (const dataset of datasets) {
    days.set(dataset.first_day, dataset.contract.price_increment);
    for (const change of dataset.price_increment_changes)
      days.set(change.trading_day, change.value);
  }
  const ordered = [...days].sort(([a], [b]) => a.localeCompare(b));
  return ordered
    .filter(([, value], i) => i === 0 || value !== ordered[i - 1][1])
    .map(([day, value], i) => (i ? t("{day} 起 {tick}", { day, tick: value }) : value))
    .join("、");
}
// Months whose lots differ are each listed once.
export function multipliers(datasets: DatasetUnits[]) {
  return [...new Set(datasets.map(dataset => dataset.contract.multiplier))].join("、");
}
export function contractUnits(datasets: DatasetUnits[]) {
  return t("最小变动价位 {tick} · 合约乘数 {multiplier}", {
    tick: priceIncrements(datasets),
    multiplier: multipliers(datasets),
  });
}
