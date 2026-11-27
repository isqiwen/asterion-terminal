// LocalStorage state shared across research tabs: condition presets, symbol
// universes, built-in China futures sector pools, and experiment annotations.
import { type MessageValues, translate } from "../../src/i18n";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

export function symbolsOf(text: string) {
  return text
    .split(/[\s,，]+/)
    .map(item => item.trim())
    .filter(Boolean);
}

export function sameSymbolList(a: string, b: string) {
  const [x, y] = [symbolsOf(a), symbolsOf(b)];
  return x.length === y.length && x.every(symbol => y.includes(symbol));
}

// Saved benchmark presets plus the current ad-hoc working draft.
export type ConditionSet<F> = {
  active: string;
  current?: F;
  items: { name: string; fields: F }[];
};

function mergeConditionFields<F extends object>(
  defaults: () => F,
  raw: Record<string, unknown> | undefined,
): F {
  const fields = defaults() as Record<string, unknown>;
  for (const field of Object.keys(fields))
    if (typeof raw?.[field] === typeof fields[field]) fields[field] = raw![field];
  return fields as F;
}

export function readConditionSet<F extends object>(
  key: string,
  defaults: () => F,
): ConditionSet<F> {
  try {
    const saved = JSON.parse(localStorage.getItem(key) ?? "");
    const items: ConditionSet<F>["items"] = Array.isArray(saved.items)
      ? saved.items
          .filter((item: { name?: unknown }) => item?.name && typeof item.name === "string")
          .map((item: { name: string; fields?: Record<string, unknown> }) => ({
            name: item.name,
            fields: mergeConditionFields(defaults, item.fields),
          }))
      : [];
    const active =
      typeof saved.active === "string" && items.some(item => item.name === saved.active)
        ? saved.active
        : "";
    const current =
      saved.current && typeof saved.current === "object"
        ? mergeConditionFields(defaults, saved.current as Record<string, unknown>)
        : active
          ? items.find(item => item.name === active)?.fields
          : undefined;
    return { active, current, items };
  } catch {
    // Nothing saved yet, or a damaged entry.
    return { active: "", items: [] };
  }
}

// Shared symbol universes reused across StrategyLab and FactorLab.
export type Universe = { name: string; symbols: string };
const universesKey = "asterion.research.universes";
const universeListeners = new Set<() => void>();

export function readUniverses(): Universe[] {
  try {
    const parsed = JSON.parse(localStorage.getItem(universesKey) ?? "");
    if (!Array.isArray(parsed)) return [];
    return parsed.filter(
      (item): item is Universe =>
        Boolean(item) &&
        typeof item.name === "string" &&
        Boolean(item.name.trim()) &&
        typeof item.symbols === "string" &&
        symbolsOf(item.symbols).length > 0,
    );
  } catch {
    return [];
  }
}

export function saveUniverses(items: Universe[]) {
  localStorage.setItem(universesKey, JSON.stringify(items));
  universeListeners.forEach(listener => listener());
}

export function subscribeUniverses(listener: () => void) {
  universeListeners.add(listener);
  return () => {
    universeListeners.delete(listener);
  };
}

// Experiment annotations (stars and one-line notes) shared across runs and factor evaluations.
export type ExperimentAnnotation = { starred?: boolean; note?: string };
const annotationsKey = "asterion.research.annotations";
const annotationListeners = new Set<() => void>();

export function readAnnotations(): Record<string, ExperimentAnnotation> {
  try {
    const parsed = JSON.parse(localStorage.getItem(annotationsKey) ?? "");
    if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) return {};
    const out: Record<string, ExperimentAnnotation> = {};
    for (const [id, raw] of Object.entries(parsed as Record<string, unknown>)) {
      if (!raw || typeof raw !== "object") continue;
      const item = raw as Record<string, unknown>;
      const starred = item.starred === true;
      const note = typeof item.note === "string" ? item.note.trim() : "";
      if (starred || note)
        out[id] = { ...(starred ? { starred: true } : {}), ...(note ? { note } : {}) };
    }
    return out;
  } catch {
    return {};
  }
}

export function saveAnnotation(id: string, patch: Partial<ExperimentAnnotation>) {
  const all = readAnnotations();
  const prev = all[id] ?? {};
  const starred = patch.starred !== undefined ? Boolean(patch.starred) : Boolean(prev.starred);
  const note = patch.note !== undefined ? patch.note.trim().slice(0, 160) : (prev.note ?? "");
  if (starred || note) {
    all[id] = { ...(starred ? { starred: true } : {}), ...(note ? { note } : {}) };
  } else {
    delete all[id];
  }
  localStorage.setItem(annotationsKey, JSON.stringify(all));
  annotationListeners.forEach(listener => listener());
}

export function deleteAnnotations(ids: readonly string[]) {
  if (!ids.length) return;
  const all = readAnnotations();
  let changed = false;
  for (const id of ids) {
    if (id in all) {
      delete all[id];
      changed = true;
    }
  }
  if (!changed) return;
  localStorage.setItem(annotationsKey, JSON.stringify(all));
  annotationListeners.forEach(listener => listener());
}

export function subscribeAnnotations(listener: () => void) {
  annotationListeners.add(listener);
  return () => {
    annotationListeners.delete(listener);
  };
}

type SectorId =
  "black" | "nonferrous" | "precious" | "energy_chem" | "oilseeds" | "agriculture" | "financial";

// Mirrors asterion_strategy.PRODUCT_SECTORS so UI universe pools match backend neutralization.
const PRODUCT_SECTORS: Record<string, SectorId> = Object.fromEntries([
  ...["rb", "hc", "wr", "ss", "i", "j", "jm", "sf", "sm", "zc", "tc"].map(p => [p, "black"]),
  ...["cu", "bc", "al", "ad", "ao", "zn", "pb", "ni", "sn", "si", "ps", "lc"].map(p => [
    p,
    "nonferrous",
  ]),
  ...["au", "ag", "pt", "pd"].map(p => [p, "precious"]),
  ...[
    "sc",
    "fu",
    "lu",
    "bu",
    "ru",
    "nr",
    "br",
    "ta",
    "px",
    "pf",
    "pr",
    "pl",
    "ma",
    "me",
    "eg",
    "eb",
    "bz",
    "l",
    "pp",
    "v",
    "pg",
    "fg",
    "sa",
    "sh",
    "ur",
    "sp",
    "op",
    "ec",
  ].map(p => [p, "energy_chem"]),
  ...["a", "b", "m", "y", "p", "oi", "ro", "rm", "rs", "pk", "so"].map(p => [p, "oilseeds"]),
  ...[
    "c",
    "cs",
    "sr",
    "cf",
    "cy",
    "ap",
    "cj",
    "jd",
    "lh",
    "rr",
    "jr",
    "ri",
    "er",
    "lr",
    "wh",
    "ws",
    "wt",
    "pm",
    "bb",
    "fb",
    "lg",
  ].map(p => [p, "agriculture"]),
  ...["if", "ih", "ic", "im", "ts", "tf", "t", "tl"].map(p => [p, "financial"]),
]);

const SECTOR_TITLES: readonly [SectorId, string][] = [
  ["black", "黑色"],
  ["nonferrous", "有色"],
  ["precious", "贵金属"],
  ["energy_chem", "能化"],
  ["oilseeds", "油脂油料"],
  ["agriculture", "农副"],
  ["financial", "金融"],
];

function productSector(symbol: string): SectorId | undefined {
  const parts = symbol.trim().split("/");
  const code = (parts.length >= 2 ? parts[1] : parts[0])?.toLowerCase();
  return code ? PRODUCT_SECTORS[code] : undefined;
}

export function builtInUniverses(storedProducts: readonly string[]): Universe[] {
  const pools: Universe[] = [];
  if (storedProducts.length >= 2) {
    pools.push({ name: t("全部已入库"), symbols: storedProducts.join(", ") });
  }
  const bySector = new Map<SectorId, string[]>();
  for (const product of storedProducts) {
    const sector = productSector(product);
    if (!sector) continue;
    const list = bySector.get(sector);
    if (list) list.push(product);
    else bySector.set(sector, [product]);
  }
  for (const [sector, label] of SECTOR_TITLES) {
    const list = bySector.get(sector);
    if (list?.length) pools.push({ name: t(label), symbols: list.join(", ") });
  }
  return pools;
}
