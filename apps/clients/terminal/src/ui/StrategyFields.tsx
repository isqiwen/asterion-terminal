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
} as const;
type Kind = keyof typeof rules;
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
};

/** The definition a draft submits. */
export function strategyOf(draft: StrategyDraft): StrategyDefinition {
  const whole = (name: "fast" | "slow" | "entry" | "exit" | "lookback" | "window") =>
    Number(draft[name]);
  const rule =
    draft.kind === "moving_average"
      ? { kind: draft.kind, fast: whole("fast"), slow: whole("slow") }
      : draft.kind === "breakout"
        ? { kind: draft.kind, entry: whole("entry"), exit: whole("exit") }
        : draft.kind === "momentum"
          ? { kind: draft.kind, lookback: whole("lookback") }
          : { kind: draft.kind, window: whole("window"), width: draft.width };
  return { quantity: draft.quantity, sides: draft.sides, rule };
}

/** A strategy's rule with its windows, as one reads it in a heading. */
export function strategyRule(definition: StrategyDefinition): string {
  const rule = definition.rule;
  const windows =
    rule.kind === "moving_average"
      ? `${rule.fast}/${rule.slow}`
      : rule.kind === "breakout"
        ? `${rule.entry}/${rule.exit}`
        : rule.kind === "momentum"
          ? `${rule.lookback}`
          : `${rule.window} · ${rule.width}σ`;
  return `${t(rules[rule.kind].name)} ${windows}`;
}
export const strategySides = (value: PositionSides) => t(sides[value]);

/** A strategy's fields as labelled rows, for showing what was submitted. */
export function strategyRows(definition: StrategyDefinition): [string, string | number][] {
  const rule = definition.rule;
  const values = rule as unknown as Record<string, string | number>;
  return [
    [t("策略"), t(rules[rule.kind].name)],
    ...rules[rule.kind].fields.map(
      ([name, label]) => [t(label), values[name]] as [string, string | number],
    ),
    [t("目标手数"), definition.quantity],
    [t("持仓方向"), strategySides(definition.sides)],
  ];
}

/** The form fields of one strategy; `className` is the host's field grid. */
export function StrategyFields({
  value,
  onChange,
  className,
}: {
  value: StrategyDraft;
  onChange: (next: StrategyDraft) => void;
  className: string;
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
            {Object.entries(rules).map(([kind, item]) => (
              <option key={kind} value={kind}>
                {t(item.name)}
              </option>
            ))}
          </select>
        </label>
        {rule.fields.map(([name, label]) => (
          <label key={name}>
            {t(label)}
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
          </label>
        ))}
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
