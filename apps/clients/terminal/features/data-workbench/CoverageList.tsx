import { useState } from "react";
import {
  Table,
  type AcquisitionTask,
  type ContractCoverage,
  type ProductCoverage,
  type SeriesCoverage,
  type SeriesSummary,
} from "../../src/generated/asterion/warehouse_pb";
import { getLocale } from "../../src/i18n";
import { Dialog } from "./Dialog";
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
  if (!series?.days) return series?.emptyDays ? "empty" : "none";
  if (series.gapDays) return "gap";
  if (series.pendingDays) return "pending";
  if (series.openDays > series.days + series.emptyDays) return "early";
  return series.emptyDays ? "empty" : "full";
}

// What a contract still lacks of its life, as one daily acquisition. Days are
// stored per exchange, so a gap on a day the exchange holds has to be refetched.
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
    refetch: state === "gap",
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

// One acquisition for days to refetch and one for days never asked for.
function merged(rules: Rule[]): Rule[] {
  if (rules[0]?.table === Table.MINUTES)
    return [{ ...rules[0], instruments: rules.flatMap(rule => rule.instruments) }];
  return [true, false].flatMap(refetch => {
    const group = rules.filter(rule => rule.refetch === refetch);
    return group.length
      ? [
          {
            ...group[0]!,
            firstDay: earliest(group.map(rule => rule.firstDay)),
            lastDay: latest(group.map(rule => rule.lastDay)),
          },
        ]
      : [];
  });
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

function StateText({ state, children }: { state: State; children: string }) {
  return (
    <span className="coverage-state" data-state={state}>
      {children}
    </span>
  );
}

const seriesText = (state: State, series: SeriesCoverage | undefined) =>
  ({
    none: t("未采集"),
    full: t("齐全"),
    gap: t("缺 {days} 天", { days: series?.gapDays ?? 0 }),
    pending: t("待更新"),
    early: t("前段未采"),
    empty: t("来源无数据"),
  })[state];

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

function tallyText(tally: Tally) {
  const state = tallyState(tally);
  if (state === "none") return t("未采集");
  if (state === "gap") return t("缺 {count} 个合约", { count: tally.gaps });
  if (state === "pending") return t("待更新");
  if (state === "full") return t("齐全");
  const part = tally.held - tally.complete;
  return [
    tally.complete > 0 && t("采全 {count}", { count: tally.complete }),
    part > 0 && t("部分 {count}", { count: part }),
  ]
    .filter(Boolean)
    .join(" · ");
}

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

function Progress({ pieces }: { pieces: Piece[] }) {
  return (
    <div className="coverage-lane" aria-hidden="true">
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
  view,
}: {
  contract: ContractCoverage;
  kind: Kind;
  nested: boolean;
  running: readonly AcquisitionTask[];
  acquires: boolean;
  start: (rules: Rule[]) => void;
  view: (contract: string) => void;
}) {
  const code = contractCode(contract.contract);
  const series = contract[kind];
  const state = stateOf(series);
  const rule = acquires ? ruleOf(contract, kind) : undefined;
  const busy = !!rule && running.some(task => covers(task, rule));
  return (
    <tr data-kind="contract" data-nested={nested || undefined}>
      <th scope="row">
        <button
          type="button"
          className="coverage-contract"
          aria-label={t("查看 {code}", { code })}
          onClick={() => view(contract.contract)}
        >
          {code}
        </button>
      </th>
      <td className="coverage-listed">{contract.listDay}</td>
      <td>
        <Progress pieces={lifePieces(contract, series)} />
      </td>
      <td>
        <StateText state={state}>{seriesText(state, series)}</StateText>
      </td>
      <td>
        {busy ? (
          <span className="coverage-busy">{t("采集中")}</span>
        ) : rule ? (
          <button
            type="button"
            className="coverage-action"
            aria-label={t(state === "none" ? "采集 {name}" : "补全 {name}", { name: code })}
            onClick={() => start([rule])}
          >
            {state === "none" ? t("采集") : t("补全")}
          </button>
        ) : null}
      </td>
    </tr>
  );
}

type Confirm = { title: string; from: string; note: string; rule: Rule };

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
  view: (contract: string) => void;
}) {
  const [historyOpen, setHistoryOpen] = useState("");
  const [yearOpen, setYearOpen] = useState("");
  const [confirm, setConfirm] = useState<Confirm>();
  const rowTally = (row: ProductRow) =>
    summaryTally(row.coverage?.contracts ?? 0, row.coverage?.[kind]);
  const rank = (row: ProductRow) => {
    const state = tallyState(rowTally(row));
    return state === "gap" ? 0 : state === "pending" ? 1 : state === "none" ? 3 : 2;
  };
  const ordered = [...rows].sort(
    (a, b) =>
      rank(a) - rank(b) ||
      exchanges.indexOf(a.exchange) - exchanges.indexOf(b.exchange) ||
      a.id.localeCompare(b.id),
  );
  const searching = /\d/.test(query);
  const matches = (item: ContractCoverage) =>
    !searching || contractCode(item.contract).toUpperCase().includes(query);
  const rowProps = { kind, running, acquires, start, view };

  const askHistory = (product: string, history: ContractCoverage[], from: string, to: string) => {
    const chosen = history.filter(item => {
      const year = deliveryYear(item.contract);
      return year >= from && year <= to && !!ruleOf(item, kind);
    });
    const rules = chosen.flatMap(item => ruleOf(item, kind) ?? []);
    if (!rules.length) return;
    const years = chosen.map(item => deliveryYear(item.contract));
    const [low, high] = [earliest(years), latest(years)];
    const title = t("采集 {code} {years} 年交割的 {count} 个合约", {
      code: product.split("/")[1]!,
      years: low === high ? low : `${low}–${high}`,
      count: chosen.length,
    });
    if (kind === "minutes") {
      const requests = chosen.reduce(
        (sum, { minutes }) =>
          sum +
          Math.max(0, (minutes?.openDays ?? 0) - (minutes?.days ?? 0) - (minutes?.emptyDays ?? 0)),
        0,
      );
      setConfirm({
        title,
        from: earliest(chosen.map(item => item.listDay || throughOf(item))).slice(0, 7),
        note: t("分钟线按合约逐日采集，约 {count} 次请求", {
          count: requests.toLocaleString(getLocale()),
        }),
        rule: merged(rules)[0]!,
      });
      return;
    }
    const firstDay = earliest(rules.map(rule => rule.firstDay));
    setConfirm({
      title,
      from: firstDay.slice(0, 7),
      note: t("日线按交易所整日采集，{exchange} 其他品种这些交易日的数据也会入库", {
        exchange: product.split("/")[0]!,
      }),
      rule: {
        ...rules[0]!,
        refetch: rules.some(rule => rule.refetch),
        firstDay,
        lastDay: latest(rules.map(rule => rule.lastDay)),
      },
    });
  };

  return (
    <div className="coverage-list" role="region" aria-label={t("数据覆盖")} tabIndex={0}>
      <table className="coverage-table" aria-label={t("品种覆盖")}>
        <colgroup>
          <col className="coverage-col-name" />
          <col className="coverage-col-count" />
          <col />
          <col className="coverage-col-state" />
          <col className="coverage-col-action" />
        </colgroup>
        <thead>
          <tr>
            <th scope="col">{t("品种")}</th>
            <th scope="col">{t("在市 / 全部")}</th>
            <th scope="col">{t("采集进度")}</th>
            <th scope="col">{t("状态")}</th>
            <th scope="col" aria-label={t("操作")} />
          </tr>
        </thead>
        {ordered.map(row => {
          const expanded = open === row.id;
          const series = row.coverage?.[kind];
          const tally = rowTally(row);
          // A product with nothing listed is collected year by year from its history.
          const needsWork =
            (!!row.coverage?.listed || !!series?.withGaps) &&
            (!series?.held ||
              !!series.withGaps ||
              !!series.pending ||
              series.listedComplete < (row.coverage?.listed ?? 0));
          const busy = running.some(({ request }) =>
            kind === "daily"
              ? request!.table === Table.DAILY && request!.exchanges.includes(row.exchange)
              : request!.table === Table.MINUTES &&
                request!.instruments.some(item => item === row.id || item.startsWith(`${row.id}/`)),
          );
          const items = expanded ? (contracts ?? []) : [];
          const listed = items.filter(item => item.listed && matches(item));
          // A historical contract with a gap stays in view; the rest fold by delivery year.
          const history = items.filter(item => !item.listed);
          const flagged = history.filter(item => stateOf(item[kind]) === "gap" && matches(item));
          const years = [...new Set(history.map(item => deliveryYear(item.contract)))].sort(
            (a, b) => b.localeCompare(a),
          );
          const historyTally = contractsTally(history, kind);
          const searchedYears = searching
            ? new Set(history.filter(matches).map(item => deliveryYear(item.contract)))
            : undefined;
          const showHistory = !!searchedYears?.size || historyOpen === row.id;
          const newest = years[0] ?? "";
          return (
            <tbody key={row.id}>
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
                  <Progress pieces={tallyPieces(tally)} />
                </td>
                <td>
                  <StateText state={tallyState(tally)}>{tallyText(tally)}</StateText>
                </td>
                <td>
                  {!acquires ? null : busy ? (
                    <span className="coverage-busy">{t("采集中")}</span>
                  ) : needsWork ? (
                    <button
                      type="button"
                      className="coverage-action"
                      aria-label={t(series?.held ? "补全 {name}" : "采集 {name}", {
                        name: row.id,
                      })}
                      onClick={() => collect(row)}
                    >
                      {series?.held ? t("补全") : t("采集")}
                    </button>
                  ) : null}
                </td>
              </tr>
              {expanded && !contracts && (
                <tr data-kind="note">
                  <td colSpan={5}>{t("读取中")}</td>
                </tr>
              )}
              {listed.map(item => (
                <ContractRow key={item.contract} contract={item} nested={false} {...rowProps} />
              ))}
              {flagged.map(item => (
                <ContractRow key={item.contract} contract={item} nested={false} {...rowProps} />
              ))}
              {history.length > 0 && (
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
                    <Progress pieces={tallyPieces(historyTally)} />
                  </td>
                  <td>
                    <StateText state={tallyState(historyTally)}>
                      {tallyText(historyTally)}
                    </StateText>
                  </td>
                  <td />
                </tr>
              )}
              {showHistory &&
                years
                  .filter(year => !searchedYears || searchedYears.has(year))
                  .map(year => {
                    const inYear = history.filter(item => deliveryYear(item.contract) === year);
                    const yearTally = contractsTally(inYear, kind);
                    const rules = inYear.flatMap(item => ruleOf(item, kind) ?? []);
                    const yearKey = `${row.id}/${year}`;
                    const shown = yearOpen === yearKey || !!searchedYears;
                    const yearBusy = rules.some(rule => running.some(task => covers(task, rule)));
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
                          <Progress pieces={tallyPieces(yearTally)} />
                        </td>
                        <td>
                          <StateText state={tallyState(yearTally)}>
                            {tallyText(yearTally)}
                          </StateText>
                        </td>
                        <td>
                          {!acquires || !rules.length ? null : yearBusy ? (
                            <span className="coverage-busy">{t("采集中")}</span>
                          ) : (
                            <button
                              type="button"
                              className="coverage-action"
                              aria-label={t(yearTally.held ? "补全 {name}" : "采到 {name}", {
                                name: `${row.id} ${year}`,
                              })}
                              onClick={() =>
                                askHistory(row.id, history, year, yearTally.held ? year : newest)
                              }
                            >
                              {yearTally.held ? t("补全") : t("采到这年")}
                            </button>
                          )}
                        </td>
                      </tr>,
                      ...(shown
                        ? inYear
                            .filter(matches)
                            .map(item => (
                              <ContractRow
                                key={item.contract}
                                contract={item}
                                nested
                                {...rowProps}
                              />
                            ))
                        : []),
                    ];
                  })}
            </tbody>
          );
        })}
      </table>
      {confirm && (
        <Dialog title={confirm.title} close={() => setConfirm(undefined)}>
          <p className="coverage-confirm">
            {t("数据大约从 {month} 开始", { month: confirm.from })}
          </p>
          <p className="subtle">{confirm.note}</p>
          <div className="data-actions">
            <button type="button" onClick={() => setConfirm(undefined)}>
              {t("取消")}
            </button>
            <button
              type="button"
              className="primary"
              onClick={() => {
                start([confirm.rule]);
                setConfirm(undefined);
              }}
            >
              {t("开始采集")}
            </button>
          </div>
        </Dialog>
      )}
    </div>
  );
}
