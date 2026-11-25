import { useMemo, useState } from "react";
import {
  Table,
  type AcquisitionTask,
  type ContractCoverage,
  type ProductCoverage,
  type SeriesCoverage,
  type SeriesSummary,
} from "../../src/generated/asterion/warehouse_pb";
import { beijingDay, exchanges, t, type startAcquisition } from "./dataset";

export type Rule = Parameters<typeof startAcquisition>[0];
export type Kind = "daily" | "minutes";
export type ProductRow = {
  id: string;
  name: string;
  exchange: string;
  coverage?: ProductCoverage;
};
type State = "none" | "full" | "gap" | "pending" | "early" | "empty";

const earliest = (days: string[]) => days.reduce((a, b) => (b < a ? b : a));
const latest = (days: string[]) => days.reduce((a, b) => (b > a ? b : a));
export const contractCode = (id: string) => {
  const [, product = "", month = ""] = id.split("/");
  return `${product}${month.slice(2, 4)}${month.slice(5, 7)}`;
};
const deliveryYear = (id: string) => id.split("/")[2]!.slice(0, 4);
const throughOf = (contract: ContractCoverage) =>
  contract.delistDay && contract.delistDay < beijingDay(-1) ? contract.delistDay : beijingDay(-1);

function stateOf(series: SeriesCoverage | undefined): State {
  if (!series?.days)
    return series?.openDays && series.emptyDays >= series.openDays ? "empty" : "none";
  if (series.gapDays) return "gap";
  if (series.pendingDays) return "pending";
  if (series.openDays > series.days + series.emptyDays) return "early";
  return series.emptyDays ? "empty" : "full";
}

// What a contract still lacks of its life, as one daily acquisition. Only
// unstored exchange days count as gaps, so refetching held days is unnecessary.
function dailyRule(contract: ContractCoverage): Rule | undefined {
  const series = contract.daily;
  const state = stateOf(series);
  if (state === "full" || state === "empty") return undefined;
  const through = throughOf(contract);
  const firstDay =
    state === "gap"
      ? series!.gaps[0]!.first
      : state === "pending"
        ? series!.lastDay
        : contract.listDay || through;
  const lastDay =
    state === "early"
      ? series!.firstDay
      : state === "gap" && !series!.pendingDays
        ? series!.gaps.at(-1)!.last
        : through;
  if (firstDay > lastDay) return undefined;
  return {
    table: Table.DAILY,
    refetch: false,
    exchanges: [contract.contract.split("/")[0]!],
    instruments: [],
    firstDay,
    lastDay,
  };
}

// Minutes are stored per contract and day: the contract's whole life, skipping what is held.
function minuteRule(contract: ContractCoverage): Rule | undefined {
  const state = stateOf(contract.minutes);
  return state === "full" || state === "empty"
    ? undefined
    : {
        table: Table.MINUTES,
        refetch: false,
        exchanges: [],
        instruments: [contract.contract],
        firstDay: "",
        lastDay: "",
      };
}

const ruleOf = (contract: ContractCoverage, kind: Kind) =>
  kind === "daily" ? dailyRule(contract) : minuteRule(contract);

// One acquisition spanning every requested contract or daily range per exchange.
function merged(rules: Rule[]): Rule[] {
  if (!rules.length) return [];
  if (rules[0]!.table === Table.MINUTES)
    return [{ ...rules[0]!, instruments: rules.flatMap(rule => rule.instruments) }];
  const byExchange = new Map<string, Rule[]>();
  for (const rule of rules) {
    const key = rule.exchanges[0]!;
    const group = byExchange.get(key);
    if (group) group.push(rule);
    else byExchange.set(key, [rule]);
  }
  return [...byExchange.values()].map(group => ({
    ...group[0]!,
    firstDay: earliest(group.map(rule => rule.firstDay)),
    lastDay: latest(group.map(rule => rule.lastDay)),
  }));
}

// A product's listed contracts, and any older one with a gap in what it holds.
export const productRules = (contracts: readonly ContractCoverage[], kind: Kind) =>
  merged(
    contracts
      .filter(item => item.listed || ["gap", "pending"].includes(stateOf(item[kind])))
      .flatMap(item => ruleOf(item, kind) ?? []),
  );

const covers = (task: AcquisitionTask, rule: Rule) => {
  const asked = task.request!;
  const target = rule.instruments.length
    ? asked.instruments.some(item =>
        rule.instruments.some(wanted => wanted === item || wanted.startsWith(`${item}/`)),
      )
    : asked.exchanges.some(item => rule.exchanges.includes(item));
  return (
    asked.table === rule.table &&
    target &&
    (!asked.firstDay || !rule.lastDay || asked.firstDay <= rule.lastDay) &&
    (!asked.lastDay || !rule.firstDay || rule.firstDay <= asked.lastDay)
  );
};

// A product or delivery year counted by its contracts.
type Tally = { total: number; held: number; complete: number; gaps: number; pending: number };

const summaryTally = (total: number, series: SeriesSummary | undefined): Tally => ({
  total,
  held: series?.held ?? 0,
  complete: series?.complete ?? 0,
  gaps: series?.withGaps ?? 0,
  pending: series?.pending ?? 0,
});

const contractsTally = (items: readonly ContractCoverage[], kind: Kind): Tally => {
  const states = items.filter(item => item[kind]?.days).map(item => stateOf(item[kind]));
  return {
    total: items.length,
    held: states.length,
    complete: states.filter(state => state === "full" || state === "empty").length,
    gaps: states.filter(state => state === "gap").length,
    pending: states.filter(state => state === "pending").length,
  };
};

const tallyState = ({ total, held, complete, gaps, pending }: Tally): State =>
  !held ? "none" : gaps ? "gap" : pending ? "pending" : complete === total ? "full" : "early";

const dayMs = 86_400_000;
const nextDay = (day: string) => new Date(Date.parse(day) + dayMs).toISOString().slice(0, 10);
type Piece = { part: string; left: number; width: number };

// A contract's life, listing to delisting, is 100%; each stretch's length is its share of it.
function lifePieces(contract: ContractCoverage, series: SeriesCoverage | undefined): Piece[] {
  const start = contract.listDay || series?.firstDay;
  const end = contract.delistDay || series?.lastDay;
  if (!start || !end) return [];
  const from = Date.parse(start);
  const span = Date.parse(end) + dayMs - from;
  const edge = (time: number) => Math.min(1, Math.max(0, (time - from) / span));
  const piece = (part: string, first: string, last: string): Piece[] => {
    const left = edge(Date.parse(first));
    const right = edge(Date.parse(last) + dayMs);
    return right > left ? [{ part, left, width: right - left }] : [];
  };
  const yesterday = beijingDay(-1);
  const through = end < yesterday ? end : yesterday;
  const pieces = end > yesterday ? piece("future", nextDay(yesterday), end) : [];
  if (!series?.days)
    return series?.emptyDays && series.emptyDays >= series.openDays
      ? [...pieces, ...piece("empty", start, through)]
      : pieces;
  const unasked =
    series.openDays - series.days - series.emptyDays - series.pendingDays - series.gapDays;
  // With nothing unasked, the days before the first stored one are those no source has.
  if (!unasked && series.emptyDays && start < series.firstDay)
    pieces.push(...piece("empty", start, series.firstDay));
  pieces.push(...piece("held", series.firstDay, series.lastDay));
  if (series.pendingDays) pieces.push(...piece("pending", nextDay(series.lastDay), through));
  else if (series.emptyDays && series.lastDay < through)
    pieces.push(...piece("empty", nextDay(series.lastDay), through));
  for (const gap of series.gaps) pieces.push(...piece("gap", gap.first, gap.last));
  return pieces;
}

// A group's bar: each stretch's length is its share of the group's contracts.
function tallyPieces({ total, held, complete, gaps, pending }: Tally): Piece[] {
  if (!total) return [];
  const gap = Math.min(gaps, held - complete);
  const late = Math.min(pending, held - complete - gap);
  let left = 0;
  return (
    [
      ["held", complete],
      ["part", held - complete - gap - late],
      ["gap", gap],
      ["pending", late],
    ] as const
  ).flatMap(([part, count]) => {
    const item = { part, left, width: count / total };
    left += item.width;
    return count ? [item] : [];
  });
}

function contractSummary(series: SeriesCoverage | undefined) {
  if (!series?.days) {
    if (!series?.emptyDays) return t("未采集");
    if (series.openDays > 0 && series.emptyDays >= series.openDays)
      return t("来源无数据 {days} 天", { days: series.emptyDays });
    return [t("未采集"), t("来源无数据 {days} 天", { days: series.emptyDays })].join(" · ");
  }
  const parts = [
    t("已采 {days} 天（{first}–{last}）", {
      days: series.days,
      first: series.firstDay,
      last: series.lastDay,
    }),
  ];
  const early = Math.max(
    0,
    series.openDays - series.days - series.emptyDays - series.pendingDays - series.gapDays,
  );
  if (early > 0) parts.push(t("前段未采 {days} 天", { days: early }));
  if (series.gaps.length) {
    const shown = series.gaps
      .slice(0, 3)
      .map(gap => (gap.first === gap.last ? gap.first : `${gap.first}–${gap.last}`))
      .join(", ");
    parts.push(
      t("缺口 {ranges}", {
        ranges: series.gaps.length > 3 ? `${shown} …` : shown,
      }),
    );
  }
  if (series.pendingDays) parts.push(t("待更新 {days} 天", { days: series.pendingDays }));
  if (series.emptyDays) parts.push(t("来源无数据 {days} 天", { days: series.emptyDays }));
  return parts.join(" · ");
}

function tallySummary({ total, held, complete, gaps, pending }: Tally) {
  if (!total || !held) return t("未采集");
  const part = Math.max(0, held - complete - gaps - pending);
  const parts = [t("已采全 {complete} / {total}", { complete, total })];
  if (part > 0) parts.push(t("部分采集 {count}", { count: part }));
  if (gaps > 0) parts.push(t("缺口 {count}", { count: gaps }));
  if (pending > 0) parts.push(t("待更新 {count}", { count: pending }));
  return parts.join(" · ");
}

function Progress({ pieces, label }: { pieces: Piece[]; label: string }) {
  return (
    <div className="coverage-lane" role="img" title={label} aria-label={label}>
      {pieces.map(({ part, left, width }) => (
        <i
          key={`${part}-${left}`}
          data-part={part}
          style={{
            left: `${(left * 100).toFixed(2)}%`,
            width: `${(width * 100).toFixed(2)}%`,
          }}
        />
      ))}
    </div>
  );
}

function ContractRow({
  contract,
  kind,
  nested,
  running,
  acquires,
  start,
  viewing,
  view,
}: {
  contract: ContractCoverage;
  kind: Kind;
  nested: boolean;
  running: readonly AcquisitionTask[];
  acquires: boolean;
  start: (rules: Rule[]) => void;
  viewing: string;
  view: (contract: string) => void;
}) {
  const code = contractCode(contract.contract);
  const series = contract[kind];
  const pendingRule = ruleOf(contract, kind);
  const rule = acquires ? pendingRule : undefined;
  const done = !!series?.days && !pendingRule;
  const busy = !!rule && running.some(task => covers(task, rule));
  return (
    <tr
      data-kind="contract"
      data-nested={nested || undefined}
      data-viewing={viewing === contract.contract || undefined}
    >
      <th scope="row">
        <button
          type="button"
          className="coverage-contract"
          aria-label={t("查看 {code}", { code })}
          aria-pressed={viewing === contract.contract}
          onClick={() => view(contract.contract)}
        >
          {code}
        </button>
      </th>
      <td />
      <td>
        <Progress pieces={lifePieces(contract, series)} label={contractSummary(series)} />
      </td>
      <td>
        {busy ? (
          <span className="coverage-busy">{t("采集中")}</span>
        ) : rule ? (
          <button
            type="button"
            className="coverage-action"
            aria-label={t("采集 {name}", { name: code })}
            onClick={() => start([rule])}
          >
            {t("采集")}
          </button>
        ) : done ? (
          <span className="coverage-done">{t("齐全")}</span>
        ) : null}
      </td>
    </tr>
  );
}

// Products with their contracts for one kind of data: what is held of each,
// from listing to delisting, and one action that takes what is missing.
export function CoverageList({
  kind,
  rows,
  contracts,
  open,
  setOpen,
  query,
  running,
  acquires,
  start,
  collect,
  loadContracts,
  viewing,
  view,
}: {
  kind: Kind;
  rows: readonly ProductRow[];
  // Those of the open product, once read.
  contracts: readonly ContractCoverage[] | undefined;
  open: string;
  setOpen: (product: string) => void;
  query: string;
  running: readonly AcquisitionTask[];
  acquires: boolean;
  start: (rules: Rule[]) => void;
  collect: (row: ProductRow) => void;
  loadContracts: (product: string) => Promise<ContractCoverage[] | undefined>;
  viewing: string;
  view: (contract: string) => void;
}) {
  const [retiredOpen, setRetiredOpen] = useState(false);
  const [loadingProduct, setLoadingProduct] = useState("");
  const [historyOpen, setHistoryOpen] = useState("");
  const [yearOpen, setYearOpen] = useState("");
  const rowTally = (row: ProductRow) =>
    summaryTally(row.coverage?.contracts ?? 0, row.coverage?.[kind]);
  const ordered = useMemo(() => {
    const rank = (row: ProductRow) => {
      const state = tallyState(summaryTally(row.coverage?.contracts ?? 0, row.coverage?.[kind]));
      return state === "gap" ? 0 : state === "pending" ? 1 : state === "none" ? 3 : 2;
    };
    return [...rows].sort(
      (a, b) =>
        rank(a) - rank(b) ||
        exchanges.indexOf(a.exchange) - exchanges.indexOf(b.exchange) ||
        (a.id < b.id ? -1 : a.id > b.id ? 1 : 0),
    );
  }, [rows, kind]);
  const activeRows = ordered.filter(row => !!row.coverage?.listed);
  const retiredRows = ordered.filter(row => !row.coverage?.listed);
  const retiredTally = retiredRows.reduce<Tally>(
    (acc, row) => {
      const item = rowTally(row);
      return {
        total: acc.total + item.total,
        held: acc.held + item.held,
        complete: acc.complete + item.complete,
        gaps: acc.gaps + item.gaps,
        pending: acc.pending + item.pending,
      };
    },
    { total: 0, held: 0, complete: 0, gaps: 0, pending: 0 },
  );
  const retiredIncomplete = retiredRows.filter(
    row => (row.coverage?.[kind]?.complete ?? 0) < (row.coverage?.contracts ?? 0),
  );
  const retiredDone =
    retiredTally.total > 0 &&
    retiredTally.complete >= retiredTally.total &&
    !retiredTally.gaps &&
    !retiredTally.pending;
  const productBusy = (row: ProductRow) =>
    running.some(({ request }) =>
      kind === "daily"
        ? request!.table === Table.DAILY && request!.exchanges.includes(row.exchange)
        : request!.table === Table.MINUTES &&
          request!.instruments.some(item => item === row.id || item.startsWith(`${row.id}/`)),
    );
  const retiredBusy = retiredIncomplete.some(productBusy);
  const autoExpandRetired =
    !!query || !activeRows.length || retiredRows.some(row => row.id === open);
  const showRetired = retiredOpen || autoExpandRetired;
  const searching = /\d/.test(query);
  const matches = (item: ContractCoverage) =>
    !searching || contractCode(item.contract).toUpperCase().includes(query);
  const rowProps = { kind, running, acquires, start, viewing, view };

  const startHistory = (history: readonly ContractCoverage[], from?: string, to?: string) => {
    const rules = history
      .filter(item => {
        if (!from && !to) return true;
        const year = deliveryYear(item.contract);
        return (!from || year >= from) && (!to || year <= to);
      })
      .flatMap(item => ruleOf(item, kind) ?? []);
    if (!rules.length) return false;
    start(merged(rules));
    return true;
  };

  const collectHistory = async (row: ProductRow) => {
    setLoadingProduct(row.id);
    try {
      const loaded = open === row.id && contracts ? contracts : await loadContracts(row.id);
      if (!loaded) return;
      const history = loaded.filter(item => !item.listed);
      if (!startHistory(history)) setOpen(row.id);
    } finally {
      setLoadingProduct(current => (current === row.id ? "" : current));
    }
  };

  const collectRetiredGroup = async () => {
    setLoadingProduct("__retired__");
    try {
      const loaded = await Promise.all(
        retiredIncomplete.map(row =>
          open === row.id && contracts ? Promise.resolve(contracts) : loadContracts(row.id),
        ),
      );
      const history = loaded.flatMap(items => (items ?? []).filter(item => !item.listed));
      if (!startHistory(history)) setRetiredOpen(true);
    } finally {
      setLoadingProduct(current => (current === "__retired__" ? "" : current));
    }
  };

  const renderProduct = (row: ProductRow) => {
    const contractsCount = row.coverage?.contracts ?? 0;
    const listedCount = row.coverage?.listed ?? 0;
    const pastCount = Math.max(0, contractsCount - listedCount);
    const retired = !listedCount;
    const expanded = open === row.id;
    const series = row.coverage?.[kind];
    const tally = rowTally(row);
    const items = expanded ? (contracts ?? []) : [];
    const listed = items.filter(item => item.listed && matches(item));
    // A historical contract with a gap stays in view; the rest fold by delivery year.
    const history = items.filter(item => !item.listed);
    const flagged = history.filter(item => stateOf(item[kind]) === "gap" && matches(item));
    const years = [...new Set(history.map(item => deliveryYear(item.contract)))].sort((a, b) =>
      b.localeCompare(a),
    );
    const historyTally = contractsTally(history, kind);
    const searchedYears = searching
      ? new Set(history.filter(matches).map(item => deliveryYear(item.contract)))
      : undefined;
    const showHistory = !!searchedYears?.size || historyOpen === row.id;
    const historyRules = history.flatMap(item => ruleOf(item, kind) ?? []);
    const historyBusy = historyRules.some(rule => running.some(task => covers(task, rule)));
    const historyDone =
      history.length > 0 &&
      historyTally.complete === history.length &&
      !historyTally.gaps &&
      !historyTally.pending &&
      !historyRules.length;
    const needsListed =
      !retired &&
      (!!series?.withGaps || !!series?.pending || (series?.listedComplete ?? 0) < listedCount);
    const needsHistory =
      pastCount > 0 &&
      (series?.complete ?? 0) < contractsCount &&
      (!expanded || !contracts || historyRules.length > 0);
    const productDone =
      contractsCount > 0 &&
      (series?.complete ?? 0) >= contractsCount &&
      !series?.withGaps &&
      !series?.pending;
    const busy = productBusy(row);
    return (
      <tbody key={row.id} data-retired={retired || undefined}>
        <tr data-kind="product" data-open={expanded || undefined}>
          <th scope="row">
            <button
              type="button"
              className="coverage-toggle"
              aria-expanded={expanded}
              onClick={() => setOpen(expanded ? "" : row.id)}
            >
              <code>{row.id.split("/")[1]}</code>
              <span>{row.name}</span>
              <small>{row.exchange}</small>
            </button>
          </th>
          <td className="coverage-count">
            {row.coverage ? `${row.coverage.listed} / ${row.coverage.contracts}` : "—"}
          </td>
          <td>
            <Progress pieces={tallyPieces(tally)} label={tallySummary(tally)} />
          </td>
          <td>
            {acquires && busy ? (
              <span className="coverage-busy">{t("采集中")}</span>
            ) : acquires && (needsListed || needsHistory) ? (
              <button
                type="button"
                className="coverage-action"
                disabled={!needsListed && loadingProduct === row.id}
                aria-label={t("采集 {name}", { name: row.id })}
                onClick={() => (needsListed ? collect(row) : void collectHistory(row))}
              >
                {t("采集")}
              </button>
            ) : productDone ? (
              <span className="coverage-done">{t("齐全")}</span>
            ) : null}
          </td>
        </tr>
        {expanded && !contracts && (
          <tr data-kind="note">
            <td colSpan={4}>{t("读取中")}</td>
          </tr>
        )}
        {listed.map(item => (
          <ContractRow key={item.contract} contract={item} nested={false} {...rowProps} />
        ))}
        {flagged.map(item => (
          <ContractRow key={item.contract} contract={item} nested={false} {...rowProps} />
        ))}
        {!retired && history.length > 0 && (
          <tr data-kind="history">
            <th scope="row">
              <button
                type="button"
                className="coverage-toggle"
                aria-expanded={showHistory}
                onClick={() => setHistoryOpen(historyOpen === row.id ? "" : row.id)}
              >
                <span>{t("历史合约")}</span>
              </button>
            </th>
            <td className="coverage-count">{t("{count} 个", { count: history.length })}</td>
            <td>
              <Progress pieces={tallyPieces(historyTally)} label={tallySummary(historyTally)} />
            </td>
            <td>
              {acquires && historyRules.length > 0 ? (
                historyBusy ? (
                  <span className="coverage-busy">{t("采集中")}</span>
                ) : (
                  <button
                    type="button"
                    className="coverage-action"
                    aria-label={t("采集 {name}", { name: `${row.id} ${t("历史合约")}` })}
                    onClick={() => startHistory(history)}
                  >
                    {t("采集")}
                  </button>
                )
              ) : historyDone ? (
                <span className="coverage-done">{t("齐全")}</span>
              ) : null}
            </td>
          </tr>
        )}
        {(retired || showHistory) &&
          years
            .filter(year => !searchedYears || searchedYears.has(year))
            .map(year => {
              const inYear = history.filter(item => deliveryYear(item.contract) === year);
              const yearTally = contractsTally(inYear, kind);
              const rules = inYear.flatMap(item => ruleOf(item, kind) ?? []);
              const yearKey = `${row.id}/${year}`;
              const shown = yearOpen === yearKey || !!searchedYears;
              const yearBusy = rules.some(rule => running.some(task => covers(task, rule)));
              const yearDone =
                inYear.length > 0 &&
                yearTally.complete === inYear.length &&
                !yearTally.gaps &&
                !yearTally.pending &&
                !rules.length;
              return [
                <tr key={year} data-kind="year">
                  <th scope="row">
                    <button
                      type="button"
                      className="coverage-toggle"
                      aria-expanded={shown}
                      onClick={() => setYearOpen(shown ? "" : yearKey)}
                    >
                      <span>{t("{year} 年交割", { year })}</span>
                    </button>
                  </th>
                  <td className="coverage-count">
                    {yearTally.held} / {inYear.length}
                  </td>
                  <td>
                    <Progress pieces={tallyPieces(yearTally)} label={tallySummary(yearTally)} />
                  </td>
                  <td>
                    {acquires && rules.length > 0 ? (
                      yearBusy ? (
                        <span className="coverage-busy">{t("采集中")}</span>
                      ) : (
                        <button
                          type="button"
                          className="coverage-action"
                          aria-label={t("采集 {name}", { name: `${row.id} ${year}` })}
                          onClick={() => startHistory(history, year, year)}
                        >
                          {t("采集")}
                        </button>
                      )
                    ) : yearDone ? (
                      <span className="coverage-done">{t("齐全")}</span>
                    ) : null}
                  </td>
                </tr>,
                ...(shown
                  ? inYear
                      .filter(matches)
                      .map(item => (
                        <ContractRow key={item.contract} contract={item} nested {...rowProps} />
                      ))
                  : []),
              ];
            })}
      </tbody>
    );
  };

  return (
    <div className="coverage-list" role="region" aria-label={t("采集进度")} tabIndex={0}>
      <table className="coverage-table" aria-label={t("品种采集进度")}>
        <colgroup>
          <col className="coverage-col-name" />
          <col className="coverage-col-count" />
          <col />
          <col className="coverage-col-action" />
        </colgroup>
        <thead>
          <tr>
            <th scope="col">{t("品种")}</th>
            <th scope="col">{t("在市 / 全部")}</th>
            <th scope="col">{t("采集进度")}</th>
            <th scope="col" aria-label={t("操作")} />
          </tr>
        </thead>
        {activeRows.map(renderProduct)}
        {retiredRows.length > 0 && (
          <tbody key="retired-group">
            <tr data-kind="retired-group" data-open={showRetired || undefined}>
              <th scope="row">
                <button
                  type="button"
                  className="coverage-toggle"
                  aria-expanded={showRetired}
                  onClick={() => {
                    if (showRetired && retiredRows.some(row => row.id === open)) setOpen("");
                    setRetiredOpen(!showRetired);
                  }}
                >
                  <span>{t("已退市品种")}</span>
                </button>
              </th>
              <td className="coverage-count">{t("{count} 个", { count: retiredRows.length })}</td>
              <td>
                <Progress pieces={tallyPieces(retiredTally)} label={tallySummary(retiredTally)} />
              </td>
              <td>
                {acquires && retiredBusy ? (
                  <span className="coverage-busy">{t("采集中")}</span>
                ) : acquires && retiredIncomplete.length > 0 ? (
                  <button
                    type="button"
                    className="coverage-action"
                    disabled={loadingProduct === "__retired__"}
                    aria-label={t("采集 {name}", { name: t("已退市品种") })}
                    onClick={() => void collectRetiredGroup()}
                  >
                    {t("采集")}
                  </button>
                ) : retiredDone ? (
                  <span className="coverage-done">{t("齐全")}</span>
                ) : null}
              </td>
            </tr>
          </tbody>
        )}
        {showRetired && retiredRows.map(renderProduct)}
      </table>
    </div>
  );
}
