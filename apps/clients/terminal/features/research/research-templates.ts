// Python source generators for new modules, copies, factor strategies and combined portfolios.
import type { FactorResult, RunDetail } from "../../src/generated/asterion/lab_pb";

export function strategyClassName(name: string) {
  return name.replace(/(^|_)([a-zA-Z0-9])/g, (_, __, letter: string) => letter.toUpperCase());
}

function pythonLiteral(value: { value: { case?: string; value?: unknown } }): string {
  switch (value.value.case) {
    case "flag":
      return value.value.value ? "True" : "False";
    case "integer":
      return String(value.value.value);
    case "number": {
      const n = Number(value.value.value);
      return Number.isInteger(n) ? `${n}.0` : String(n);
    }
    case "text":
      return JSON.stringify(String(value.value.value));
    default:
      return "None";
  }
}

export function factorStrategyStem(result: FactorResult) {
  const names = (
    result.factorNames.length ? result.factorNames : [result.evaluation?.factor ?? "factor"]
  )
    .map(id => id.split(".")[1] ?? id)
    .map(part => part.replace(/[^A-Za-z0-9_]/g, "_"))
    .filter(Boolean);
  const raw = `${names.slice(0, 2).join("_") || "factor"}_strategy`
    .replace(/^[^A-Za-z_]+/, "")
    .replace(/_+/g, "_");
  return (raw || "factor_strategy").slice(0, 40).replace(/_+$/, "") || "factor_strategy";
}

// Generate a runnable, deployable cross-sectional Strategy from an evaluated factor (or composite).
export function factorStrategyTemplate(result: FactorResult, moduleName: string) {
  const evaluation = result.evaluation;
  const primaryId = evaluation?.factor || result.factorNames[0] || "price.momentum";
  const specs: Array<{
    id: string;
    module: string;
    fn: string;
    params: Record<string, { value: { case?: string; value?: unknown } }>;
  }> = [];
  const pushSpec = (
    id: string,
    params: Record<string, { value: { case?: string; value?: unknown } }> = {},
    target = specs,
  ) => {
    const [module, fn] = id.split(".");
    if (!module || !fn) return;
    target.push({ id, module, fn, params });
  };
  pushSpec(primaryId, evaluation?.params ?? {});
  for (const extra of evaluation?.extraFactors ?? []) {
    pushSpec(extra.factor, extra.params ?? {});
  }
  if (specs.length === 1 && result.factorNames.length >= 2) {
    for (const id of result.factorNames.slice(1)) pushSpec(id, {});
  }
  if (specs.length >= 2 && evaluation?.combination === "ic_weighted") {
    throw new Error("暂不支持从 IC 加权合成报告生成策略，请先用等权合成重新评价");
  }
  const controls: typeof specs = [];
  for (const control of evaluation?.orthogonalizeAgainst ?? []) {
    pushSpec(control.factor, control.params ?? {}, controls);
  }
  const allSpecs = [...specs, ...controls];

  // Resolve unique local aliases for imported factor functions.
  const usedNames = new Set<string>(["_volatility", "orthogonalize"]);
  const aliases = allSpecs.map(spec => {
    let alias = spec.fn;
    if (usedNames.has(alias)) alias = `${spec.module}_${spec.fn}`;
    while (usedNames.has(alias)) alias = `${alias}_f`;
    usedNames.add(alias);
    return alias;
  });
  const factorImports = new Map<string, string[]>();
  allSpecs.forEach((spec, idx) => {
    const alias = aliases[idx]!;
    const clause = alias === spec.fn ? spec.fn : `${spec.fn} as ${alias}`;
    const list = factorImports.get(spec.module) ?? [];
    if (!list.includes(clause)) list.push(clause);
    factorImports.set(spec.module, list);
  });

  const composite = specs.length >= 2;
  const winsorize = Boolean(evaluation?.winsorize);
  const standardize = evaluation?.standardize || (composite ? "zscore" : "");
  const sectorNeutral = evaluation?.neutralize === "sector";
  const strategyImports = ["WEIGHT_STEP", "Strategy", "liquid_symbols"];
  if (winsorize) strategyImports.push("winsorize_mad");
  if (standardize === "zscore") strategyImports.push("cross_section_zscore");
  if (standardize === "rank") strategyImports.push("cross_section_rank");
  if (sectorNeutral) strategyImports.push("sector_neutralize");
  if (controls.length) strategyImports.push("orthogonalize");
  strategyImports.push("cap_sector_weights", "smooth_weights");

  let maxWindow = 20;
  for (const spec of allSpecs) {
    for (const val of Object.values(spec.params)) {
      if (val.value.case === "integer") {
        const n = Number(val.value.value);
        if (Number.isFinite(n) && n > maxWindow && n <= 500) maxWindow = n;
      }
    }
  }
  const factorNeed = maxWindow + 2;
  const rebalance = evaluation?.horizons[0] ? Math.max(1, evaluation.horizons[0]) : 5;
  const groups = Math.max(2, evaluation?.groups || 5);
  const symbolCount = result.symbols.length || evaluation?.symbols.length || 6;
  const tailCount = Math.max(1, Math.min(3, Math.floor(symbolCount / groups)));
  const firstMean = result.horizons[0]?.mean;
  const direction = firstMean !== undefined && firstMean < 0 ? -1 : 1;
  const minVolume = evaluation?.minVolume ? evaluation.minVolume : 0.0;
  const minOpenInterest = evaluation?.minOpenInterest ? evaluation.minOpenInterest : 0.0;

  const cls = strategyClassName(moduleName);
  const summary = specs.map(spec => spec.id).join(" + ");
  const importLines = [
    ...[...factorImports.entries()]
      .sort(([a], [b]) => a.localeCompare(b))
      .map(([mod, items]) => `from factors.${mod} import ${items.join(", ")}`),
    "from factors.price import volatility as _volatility",
  ];

  const scoreLines: string[] = [];
  if (composite) {
    scoreLines.push("        components = []");
    specs.forEach((spec, idx) => {
      const alias = aliases[idx]!;
      const kwargs = Object.entries(spec.params)
        .map(([k, v]) => `, ${k}=${pythonLiteral(v)}`)
        .join("");
      scoreLines.push(
        `        part_${idx} = {s: float(ctx.factor(${alias}, s, self.need${kwargs})) for s in eligible}`,
      );
      if (winsorize) scoreLines.push(`        part_${idx} = winsorize_mad(part_${idx})`);
      scoreLines.push(
        `        part_${idx} = ${standardize === "rank" ? "cross_section_rank" : "cross_section_zscore"}(part_${idx})`,
      );
      scoreLines.push(`        components.append(part_${idx})`);
    });
    scoreLines.push("        scores = {}");
    scoreLines.push("        for s in eligible:");
    scoreLines.push("            vals = [c[s] for c in components if s in c]");
    scoreLines.push("            if vals:");
    scoreLines.push("                scores[s] = sum(vals) / len(vals)");
    if (sectorNeutral) scoreLines.push("        scores = sector_neutralize(scores)");
  } else {
    const spec = specs[0]!;
    const alias = aliases[0]!;
    const kwargs = Object.entries(spec.params)
      .map(([k, v]) => `, ${k}=${pythonLiteral(v)}`)
      .join("");
    scoreLines.push(
      `        scores = {s: float(ctx.factor(${alias}, s, self.need${kwargs})) for s in eligible}`,
    );
    scoreLines.push("        scores = {s: v for s, v in scores.items() if np.isfinite(v)}");
    if (winsorize) scoreLines.push("        scores = winsorize_mad(scores)");
    if (standardize === "zscore") scoreLines.push("        scores = cross_section_zscore(scores)");
    if (standardize === "rank") scoreLines.push("        scores = cross_section_rank(scores)");
    if (sectorNeutral) scoreLines.push("        scores = sector_neutralize(scores)");
  }
  if (controls.length) {
    scoreLines.push("        controls = [");
    controls.forEach((spec, idx) => {
      const alias = aliases[specs.length + idx]!;
      const kwargs = Object.entries(spec.params)
        .map(([k, v]) => `, ${k}=${pythonLiteral(v)}`)
        .join("");
      scoreLines.push(
        `            {s: float(ctx.factor(${alias}, s, self.need${kwargs})) for s in eligible},`,
      );
    });
    scoreLines.push("        ]", "        scores = orthogonalize(scores, controls)");
  }

  const sourceLine = result.id ? `\n    source_evaluation = ${JSON.stringify(result.id)}` : "";
  const content = `from decimal import Decimal

import numpy as np

from asterion_strategy import ${strategyImports.join(", ")}
${importLines.join("\n")}


class ${cls}(Strategy):
    """Cross-sectional inverse-volatility portfolio generated from ${summary}."""${sourceLine}

    params = {
        "rebalance": ${rebalance},
        "count": ${tailCount},
        "gross_weight": 0.6,
        "max_sector_gross": 0.4,
        "smooth_alpha": 1.0,
        "volatility": 20,
        "direction": ${direction},
        "min_volume": ${Number.isInteger(minVolume) ? `${minVolume}.0` : minVolume},
        "min_open_interest": ${Number.isInteger(minOpenInterest) ? `${minOpenInterest}.0` : minOpenInterest},
    }

    def __init__(self, **values):
        super().__init__(**values)
        p = self.params
        if (
            p["rebalance"] < 1
            or p["count"] < 1
            or p["gross_weight"] <= 0
            or p["max_sector_gross"] < 0
            or not (0.0 < p["smooth_alpha"] <= 1.0)
            or p["volatility"] < 2
            or p["direction"] not in (-1, 1)
            or p["min_volume"] < 0
            or p["min_open_interest"] < 0
        ):
            raise ValueError("invalid factor strategy parameters")
        self.need = max(${factorNeed}, p["volatility"] + 1)
        self._steps = 0
        self._prev_weights = {}

    def ready(self, ctx):
        required = min(len(ctx.symbols), 2 * self.params["count"])
        warm = sum(len(ctx.history(s, "close", self.need)) == self.need for s in ctx.symbols)
        return warm >= required

    def on_bars(self, ctx, bars):
        if not self.ready(ctx):
            return
        self._steps += 1
        p = self.params
        if (self._steps - 1) % p["rebalance"]:
            return
        eligible = liquid_symbols(
            ctx,
            bars,
            min_bars=self.need,
            min_volume=p["min_volume"],
            min_open_interest=p["min_open_interest"],
        )
        risk = {
            s: float(ctx.factor(_volatility, s, p["volatility"] + 1, window=p["volatility"]))
            for s in eligible
        }
        eligible = [s for s in eligible if np.isfinite(risk[s]) and risk[s] > 0]
${scoreLines.join("\n")}
        scores = {s: p["direction"] * v for s, v in scores.items() if np.isfinite(v)}
        ordered = sorted(scores, key=scores.get, reverse=True)
        count = p["count"]
        target = dict.fromkeys(ctx.symbols, 0.0)
        if len(ordered) < 2 * count or scores[ordered[count - 1]] <= scores[ordered[-count]]:
            self._prev_weights = dict(target)
            ctx.weights(target)
            return
        budget = int(Decimal(str(p["gross_weight"])) / (2 * WEIGHT_STEP))
        for side_symbols, side in ((ordered[:count], 1), (ordered[-count:], -1)):
            total = sum(1.0 / risk[s] for s in side_symbols)
            remaining = budget
            for i, s in enumerate(side_symbols):
                units = remaining if i == len(side_symbols) - 1 else int(budget / risk[s] / total)
                target[s] = side * float(units * WEIGHT_STEP)
                remaining -= units
        if p["max_sector_gross"] > 0:
            target = cap_sector_weights(target, max_sector_gross=p["max_sector_gross"])
        if p["smooth_alpha"] < 1.0 and self._prev_weights:
            target = smooth_weights(self._prev_weights, target, alpha=p["smooth_alpha"])
        self._prev_weights = dict(target)
        ctx.weights(target)
`;
  return { content, id: `${moduleName}.${cls}` };
}

export function copyStem(symbol: string | undefined, path: string) {
  if (!symbol) return `${path.replace(/^(strategies|factors)\//, "").replace(/\.py$/, "")}_copy`;
  const snake = symbol
    .replace(/([a-z0-9])([A-Z])/g, "$1_$2")
    .replace(/([A-Z]+)([A-Z][a-z])/g, "$1_$2")
    .toLowerCase();
  return `${snake}_copy`;
}

export function copiedModule(
  folder: "strategies" | "factors",
  content: string,
  symbol: string | undefined,
  name: string,
) {
  if (!symbol) return { content };
  const target = folder === "strategies" ? strategyClassName(name) : name;
  const pattern =
    folder === "strategies"
      ? /^class\s+([A-Za-z_][A-Za-z0-9_]*)\b/gm
      : /^def\s+([A-Za-z_][A-Za-z0-9_]*)\b/gm;
  const siblings = [...content.matchAll(pattern)]
    .map(match => match[1]!)
    .filter(item => item !== symbol && !item.startsWith("_"));
  let next = content;
  for (const other of siblings) {
    next = next.replace(new RegExp(`\\b${other}\\b`, "g"), `_${other}`);
  }
  if (target !== symbol) {
    next = next.replace(new RegExp(`\\b${symbol}\\b`, "g"), target);
  }
  return { content: next, id: `${name}.${target}` };
}

export function combinedStrategyStem(details: readonly RunDetail[]) {
  const parts = details
    .slice(0, 2)
    .map(detail =>
      (detail.summary?.strategy.split(".")[1] ?? "strategy")
        .replace(/([a-z0-9])([A-Z])/g, "$1_$2")
        .toLowerCase()
        .replace(/[^a-z0-9_]/g, "_"),
    )
    .filter(Boolean);
  const stem = `${parts.join("_")}_portfolio`.replace(/_+/g, "_").replace(/^_+|_+$/g, "");
  return /^[a-z_]/.test(stem) ? stem : `combined_${stem || "portfolio"}`;
}

export function combinedStrategyTemplate(
  details: readonly RunDetail[],
  weights: readonly number[],
  volBalance: boolean,
  moduleName: string,
): { content: string; id: string } {
  const cls = strategyClassName(moduleName);
  const imports: string[] = [];
  const paramLines: string[] = [];
  const componentLines: string[] = [];
  const names: string[] = [];
  const usedAliases = new Set<string>();

  details.forEach((detail, idx) => {
    const rec = JSON.parse(detail.record) as {
      strategy?: { id?: string };
      params?: Record<string, unknown>;
    };
    const stratId = rec.strategy?.id || detail.summary?.strategy || "";
    const [mod, stratCls] = stratId.split(".");
    if (!mod || !stratCls) return;
    let alias = stratCls;
    if (usedAliases.has(alias)) alias = `${stratCls}_${idx + 1}`;
    usedAliases.add(alias);
    imports.push(
      alias === stratCls
        ? `from strategies.${mod} import ${stratCls}`
        : `from strategies.${mod} import ${stratCls} as ${alias}`,
    );
    const share = Number((weights[idx] ?? 1 / details.length).toFixed(4));
    const shareKey = `share_${idx + 1}`;
    paramLines.push(`        "${shareKey}": ${share},`);
    const kwargs = Object.entries(rec.params ?? {})
      .filter(([, value]) => ["boolean", "number", "string"].includes(typeof value))
      .map(
        ([k, v]) => `${k}=${typeof v === "boolean" ? (v ? "True" : "False") : JSON.stringify(v)}`,
      )
      .join(", ");
    componentLines.push(`            (${alias}(${kwargs}), self.params["${shareKey}"]),`);
    names.push(stratId);
  });

  const sourceIds = details
    .map(detail => detail.summary?.id)
    .filter((id): id is string => Boolean(id));
  const sourceRunsLine = sourceIds.length
    ? `\n    source_runs = (${sourceIds.map(id => JSON.stringify(id)).join(", ")}${sourceIds.length === 1 ? "," : ""})`
    : "";
  const content = `from asterion_strategy import CombinedStrategy

${imports.join("\n")}


class ${cls}(CombinedStrategy):
    """Multi-strategy portfolio combining ${names.join(" + ")}."""${sourceRunsLine}

    vol_balance = ${volBalance ? "True" : "False"}
    max_sector_gross = 0.5
    params = {
${paramLines.join("\n")}
    }

    def components(self):
        return [
${componentLines.join("\n")}
        ]
`;
  return { content, id: `${moduleName}.${cls}` };
}

// The first content of a new module of strategies/ or factors/, which runs as it is.
export function moduleTemplate(folder: "strategies" | "factors", name: string) {
  if (folder === "factors")
    return `import numpy as np


def ${name}(close, window: int = 20):
    """Return over the last \`window\` bars."""
    result = np.full(len(close), np.nan)
    result[window:] = close[window:] / close[:-window] - 1
    return result
`;
  const cls = strategyClassName(name);
  return `from asterion_strategy import Strategy

from factors.price import momentum


class ${cls}(Strategy):
    """Long while the return over \`lookback\` bars is positive, short while it is negative."""
    params = {"lookback": 20, "lots": 1}

    def ready(self, ctx):
        need = self.params["lookback"] + 1
        return all(len(ctx.history(s, "close", need)) == need for s in ctx.symbols)

    def on_bar(self, ctx, bar):
        lookback = self.params["lookback"]
        value = ctx.factor(momentum, bar.symbol, lookback + 1, lookback=lookback)
        if value > 0:
            ctx.target(bar.symbol, self.params["lots"])
        elif value < 0:
            ctx.target(bar.symbol, -self.params["lots"])
`;
}
