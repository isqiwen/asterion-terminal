import type { DatasetSelection, FixedWeightStrategyDefinition } from "../../src/bridge/client";
import { t } from "./shared";

export type PortfolioPolicyDraft = {
  enabled: boolean;
  weights: Record<string, string>;
  gross: string;
  net: string;
  rebalance: string;
};
export const portfolioPolicyDefaults: PortfolioPolicyDraft = {
  enabled: false,
  weights: {},
  gross: "",
  net: "",
  rebalance: "",
};
const units = (value: string): bigint | null => {
  const match = /^(-?)(\d+)(?:\.(\d{1,8}))?$/.exec(value.trim());
  if (!match) return null;
  const result =
    (match[1] ? -1n : 1n) *
    (BigInt(match[2]) * 100000000n + BigInt((match[3] ?? "").padEnd(8, "0")));
  return result < -9223372036854775808n || result > 9223372036854775807n ? null : result;
};
export function portfolioEligible(datasets: DatasetSelection[], hasSeries: boolean): boolean {
  return (
    !hasSeries &&
    datasets.length >= 2 &&
    datasets.length <= 20 &&
    datasets.every(
      d =>
        d.interval_minutes === 0 &&
        d.count >= 2 &&
        units(d.contract.quantity_increment) === 100000000n &&
        d.contract.currency === datasets[0].contract.currency,
    )
  );
}
export function portfolioPolicy(
  draft: PortfolioPolicyDraft,
  datasets: DatasetSelection[],
  eligible: boolean,
): FixedWeightStrategyDefinition | null {
  if (!draft.enabled || !eligible) return null;
  const gross = units(draft.gross),
    net = units(draft.net);
  if (
    gross === null ||
    net === null ||
    gross < 0n ||
    net < 0n ||
    !/^\d+$/.test(draft.rebalance.trim())
  )
    return null;
  const rebalance = Number(draft.rebalance);
  if (!Number.isSafeInteger(rebalance) || rebalance < 1 || rebalance > 10000) return null;
  let total = 0n,
    signed = 0n;
  const members: FixedWeightStrategyDefinition["rule"]["members"] = [];
  for (const d of datasets) {
    const weight = draft.weights[`${d.venue}/${d.symbol}`]?.trim() ?? "";
    const parsed = units(weight);
    if (parsed === null) return null;
    total += parsed < 0n ? -parsed : parsed;
    signed += parsed;
    members.push({ venue: d.venue, symbol: d.symbol, weight });
  }
  if (total > gross || (signed < 0n ? -signed : signed) > net) return null;
  return {
    sides: "both",
    rule: {
      kind: "fixed_weights",
      rebalance,
      gross_weight_limit: draft.gross.trim(),
      net_weight_limit: draft.net.trim(),
      members,
    },
  };
}
export function PortfolioPolicyEditor({
  draft,
  datasets,
  eligible,
  disabled,
  onChange,
}: {
  draft: PortfolioPolicyDraft;
  datasets: DatasetSelection[];
  eligible: boolean;
  disabled: boolean;
  onChange: (value: PortfolioPolicyDraft) => void;
}) {
  return (
    <section aria-label={t("固定权重组合政策")}>
      <label>
        <input
          type="checkbox"
          checked={draft.enabled}
          disabled={disabled || (!eligible && !draft.enabled)}
          onChange={e => onChange({ ...draft, enabled: e.target.checked })}
        />
        {t("使用固定权重组合政策（仅完整普通日线）")}
      </label>
      <p>
        {t(
          "此政策替代普通CTA规则；编辑工作区政策不创建任务，明确开始回测后才接纳。不提供实盘运行授权。",
        )}
      </p>
      {draft.enabled && (
        <>
          <p>
            {t(
              "每个共同收盘边界用当时账户权益与明确有符号权重生成整数目标，向零取整；余量不计现金、不自动转配，最早后续bar成交。",
            )}
          </p>
          {!eligible && (
            <p role="alert">
              {t("需要2至20个同币种、整数步长的普通日线合约；不支持主连、分钟线或单合约候选绑定。")}
            </p>
          )}
          <fieldset disabled={disabled}>
            {datasets.map(d => (
              <label key={`${d.venue}/${d.symbol}`}>
                {t("原合约名义权重 {contract}", { contract: `${d.venue}/${d.symbol}` })}
                <input
                  inputMode="decimal"
                  value={draft.weights[`${d.venue}/${d.symbol}`] ?? ""}
                  onChange={e =>
                    onChange({
                      ...draft,
                      weights: { ...draft.weights, [`${d.venue}/${d.symbol}`]: e.target.value },
                    })
                  }
                />
              </label>
            ))}
            <label>
              {t("组合总绝对名义权重预算")}
              <input
                inputMode="decimal"
                value={draft.gross}
                onChange={e => onChange({ ...draft, gross: e.target.value })}
              />
            </label>
            <label>
              {t("组合净名义权重预算")}
              <input
                inputMode="decimal"
                value={draft.net}
                onChange={e => onChange({ ...draft, net: e.target.value })}
              />
            </label>
            <label>
              {t("每隔共同完整bar调仓（1–10000，必填）")}
              <input
                inputMode="numeric"
                value={draft.rebalance}
                onChange={e => onChange({ ...draft, rebalance: e.target.value })}
              />
            </label>
          </fieldset>
          <p>
            {t(
              "正权重做多、负权重做空、明确0为归零目标；缺失权重不补0。预算超限拒绝，不归一化或缩量。",
            )}
          </p>
          {!portfolioPolicy(draft, datasets, eligible) && (
            <p role="alert">
              {t("请明确填写全部合约权重、非负总净预算及调仓间隔；权重绝对和与净额必须在预算内。")}
            </p>
          )}
        </>
      )}
    </section>
  );
}
