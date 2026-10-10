import { translate, type MessageValues } from "../i18n";
import type { PositionSides, StrategyDefinition } from "../bridge/client";
const t = (key: string, values?: MessageValues) => translate("host", key, values);

// The rules a strategy can trade by: what each is called, the windows it
// takes, and what it does with them.
const rules = {
  moving_average: {
    name: "均线交叉",
    fields: [
      ["fast", "快均线"],
      ["slow", "慢均线"],
    ],
    about: "快均线高于慢均线时做多，低于时做空，相等时空仓。",
  },
  breakout: {
    name: "通道突破",
    fields: [
      ["entry", "入场通道"],
      ["exit", "离场通道"],
    ],
    about:
      "收盘价高于此前“入场通道”根 K 线的最高价时做多，低于其最低价时做空；收盘价反向越过此前“离场通道”根 K 线的极值时平仓。离场通道不长于入场通道。",
  },
  momentum: {
    name: "时序动量",
    fields: [["lookback", "动量回看"]],
    about: "收盘价高于“动量回看”根 K 线之前的收盘价时做多，低于时做空，相等时空仓。",
  },
  reversion: {
    name: "布林回归",
    fields: [
      ["window", "均值窗口"],
      ["width", "带宽"],
    ],
    about:
      "收盘价高于最近“均值窗口”根收盘价的均值加“带宽”倍标准差时做空，低于均值减“带宽”倍标准差时做多；收盘价回到均值时平仓。",
  },
  cross_momentum: {
    name: "截面动量",
    fields: [
      ["lookback", "动量回看"],
      ["rebalance", "调仓间隔"],
      ["count", "每侧合约数"],
    ],
    about:
      "在所有合约都有的 K 线上，每隔“调仓间隔”根按“收盘价 ÷ 动量回看根之前的收盘价”给各合约排序，最强的“每侧合约数”个做多、最弱的做空，其余空仓，持有到下一次排序。入选的合约各持有市值最接近“每个合约的名义金额”的手数，按排序那根 K 线的收盘价和合约乘数计算。一条主力连续算一个合约。",
  },
  cross_term_structure: {
    name: "截面期限结构",
    fields: [
      ["lookback", "均值窗口"],
      ["rebalance", "调仓间隔"],
      ["count", "每侧合约数"],
    ],
    about:
      "在所有品种都有期限结构的 K 线上，每隔“调仓间隔”根按最近“均值窗口”根 K 线上年化近远月价差的均值给各品种排序：价差最高（近月相对最贵）的“每侧合约数”个做多、最低的做空，其余空仓，持有到下一次排序。只用于主力连续。入选的品种各持有市值最接近“每个合约的名义金额”的手数，按排序那根 K 线的收盘价和合约乘数计算。",
  },
} as const;
type Kind = keyof typeof rules;
// Rules that rank several contracts; a run on one contract cannot use them.
const ranking: readonly Kind[] = ["cross_momentum", "cross_term_structure"];
const sides = { both: "多空", long: "只做多", short: "只做空" } as const;

// A strategy as a form holds it: every rule keeps its own windows, so
// switching rules loses nothing.
export type StrategyDraft = {
  kind: Kind;
  quantity: string;
  sides: PositionSides;
  fast: string;
  slow: string;
  entry: string;
  exit: string;
  lookback: string;
  window: string;
  width: string;
  rebalance: string;
  count: string;
  notional: string;
};
export const strategyDefaults: StrategyDraft = {
  kind: "moving_average",
  quantity: "1",
  sides: "both",
  fast: "5",
  slow: "20",
  entry: "20",
  exit: "10",
  lookback: "20",
  window: "20",
  width: "2",
  rebalance: "5",
  count: "1",
  notional: "100000",
};

const several = (text: string) =>
  text
    .split(",")
    .map(value => value.trim())
    .filter(Boolean);
// Every combination of the values a draft gives its rule's windows, in the
// order written; combinations the rule cannot work with are left out.
function combinations(draft: StrategyDraft): StrategyDefinition["rule"][] {
  const values = (
    name: "fast" | "slow" | "entry" | "exit" | "lookback" | "window" | "rebalance" | "count",
  ) => several(draft[name]).map(Number);
  if (draft.kind === "moving_average")
    return values("fast").flatMap(fast =>
      values("slow")
        .filter(slow => fast < slow)
        .map(slow => ({ kind: "moving_average" as const, fast, slow })),
    );
  if (draft.kind === "breakout")
    return values("entry").flatMap(entry =>
      values("exit")
        .filter(exit => exit <= entry)
        .map(exit => ({ kind: "breakout" as const, entry, exit })),
    );
  if (draft.kind === "momentum")
    return values("lookback").map(lookback => ({ kind: "momentum" as const, lookback }));
  if (draft.kind === "cross_momentum" || draft.kind === "cross_term_structure") {
    const kind = draft.kind;
    return values("lookback").flatMap(lookback =>
      values("rebalance").flatMap(rebalance =>
        values("count").map(count => ({
          kind,
          lookback,
          rebalance,
          count,
          notional: draft.notional,
        })),
      ),
    );
  }
  return values("window").flatMap(window =>
    several(draft.width).map(width => ({ kind: "reversion" as const, window, width })),
  );
}

/** The strategies a draft submits: one, or one for each combination of its windows. */
export function strategiesOf(draft: StrategyDraft): StrategyDefinition[] {
  // A rule that ranks contracts carries its own size and takes no quantity.
  return combinations(draft).map(rule =>
    "notional" in rule
      ? { sides: draft.sides, rule }
      : { quantity: draft.quantity, sides: draft.sides, rule },
  );
}
/** The definition a draft with one value in every window submits. */
export function strategyOf(draft: StrategyDraft): StrategyDefinition {
  return strategiesOf(draft)[0];
}

/** A strategy's rule with its windows, as one reads it in a heading. */
export function strategyRule(definition: StrategyDefinition): string {
  const rule = definition.rule;
  const windows =
    "rebalance" in rule
      ? `${rule.lookback}/${rule.rebalance}/${rule.count}`
      : rule.kind === "moving_average"
        ? `${rule.fast}/${rule.slow}`
        : rule.kind === "breakout"
          ? `${rule.entry}/${rule.exit}`
          : rule.kind === "momentum"
            ? `${rule.lookback}`
            : `${rule.window} · ${rule.width}σ`;
  return `${t(rules[rule.kind].name)} ${windows}`;
}
export const strategySides = (value: PositionSides) => t(sides[value]);
/** What a strategy holds on a side, as one reads it in a summary. */
export const strategySize = (definition: StrategyDefinition) =>
  "quantity" in definition
    ? t("{n} 手", { n: definition.quantity })
    : t("每个合约 {n}", { n: definition.rule.notional });

/** A strategy's fields as labelled rows, for showing what was submitted. */
export function strategyRows(definition: StrategyDefinition): [string, string | number][] {
  const rule = definition.rule;
  const values = rule as unknown as Record<string, string | number>;
  return [
    [t("策略"), t(rules[rule.kind].name)],
    ...rules[rule.kind].fields.map(
      ([name, label]) => [t(label), values[name]] as [string, string | number],
    ),
    "quantity" in definition
      ? [t("目标手数"), definition.quantity]
      : [t("每个合约的名义金额"), definition.rule.notional],
    [t("持仓方向"), strategySides(definition.sides)],
  ];
}

/**
 * The form fields of one strategy; `className` is the host's field grid. With
 * `compare` a window takes several comma-separated values to be compared.
 * With `portfolio` the rules that rank several contracts are offered too.
 */
export function StrategyFields({
  value,
  onChange,
  className,
  compare = false,
  portfolio = false,
}: {
  value: StrategyDraft;
  onChange: (next: StrategyDraft) => void;
  className: string;
  compare?: boolean;
  portfolio?: boolean;
}) {
  const rule = rules[value.kind];
  return (
    <>
      <div className={className}>
        <label>
          {t("策略")}
          <select
            aria-label={t("策略")}
            value={value.kind}
            onChange={event => onChange({ ...value, kind: event.target.value as Kind })}
          >
            {Object.entries(rules)
              .filter(([kind]) => portfolio || !ranking.includes(kind as Kind))
              .map(([kind, item]) => (
                <option key={kind} value={kind}>
                  {t(item.name)}
                </option>
              ))}
          </select>
        </label>
        {rule.fields.map(([name, label]) => (
          <label key={name}>
            {t(label)}
            {compare ? (
              <input
                aria-label={t(label)}
                inputMode="decimal"
                pattern={
                  name === "width"
                    ? "[ ]*[0-9]+([.][0-9]+)?[ ]*(,[ ]*[0-9]+([.][0-9]+)?[ ]*)*"
                    : "[ ]*[1-9][0-9]*[ ]*(,[ ]*[1-9][0-9]*[ ]*)*"
                }
                required
                value={value[name]}
                onChange={event => onChange({ ...value, [name]: event.target.value })}
              />
            ) : (
              <input
                aria-label={t(label)}
                type="number"
                min={name === "width" ? "0.1" : name === "slow" || name === "window" ? 2 : 1}
                max={name === "width" ? 10 : 10000}
                step={name === "width" ? "0.1" : "1"}
                required
                value={value[name]}
                onChange={event => onChange({ ...value, [name]: event.target.value })}
              />
            )}
          </label>
        ))}
        {ranking.includes(value.kind) ? (
          <label>
            {t("每个合约的名义金额")}
            <input
              aria-label={t("每个合约的名义金额")}
              inputMode="decimal"
              pattern="[0-9]+([.][0-9]+)?"
              required
              value={value.notional}
              onChange={event => onChange({ ...value, notional: event.target.value })}
            />
          </label>
        ) : (
          <label>
            {t("目标手数")}
            <input
              aria-label={t("目标手数")}
              type="number"
              min="1"
              step="1"
              required
              value={value.quantity}
              onChange={event => onChange({ ...value, quantity: event.target.value })}
            />
          </label>
        )}
        <label>
          {t("持仓方向")}
          <select
            aria-label={t("持仓方向")}
            value={value.sides}
            onChange={event => onChange({ ...value, sides: event.target.value as PositionSides })}
          >
            {Object.entries(sides).map(([side, label]) => (
              <option key={side} value={side}>
                {t(label)}
              </option>
            ))}
          </select>
        </label>
      </div>
      <p className="subtle">
        {t(rule.about)}{" "}
        {t(
          "窗口按已完成的 K 线根数计。做多或做空时持有目标手数，不允许的方向空仓；反手时先平仓，平掉之后的下一根 K 线再开仓。",
        )}
      </p>
    </>
  );
}
