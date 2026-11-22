import { useState } from "react";
import {
  Table,
  type AcquisitionTask,
  type ContractCoverage,
  type ProductCoverage,
  type SeriesCoverage,
  type SeriesSummary,
} from "../../src/generated/asterion/warehouse_pb";
import { Dialog } from "./Dialog";
import type { startAcquisition } from "./Acquisition";
import { beijingDay, exchanges, t } from "./dataset";

export type Rule = Parameters<typeof startAcquisition>[0];
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

export function stateOf(series: SeriesCoverage | undefined): State {
  if (!series?.days) return series?.emptyDays ? "empty" : "none";
  if (series.gapDays) return "gap";
  if (series.pendingDays) return "pending";
  if (series.openDays > series.days + series.emptyDays) return "early";
  return series.emptyDays ? "empty" : "full";
}

const summaryState = (series: SeriesSummary | undefined): State =>
  !series?.held ? "none" : series.withGaps ? "gap" : series.pending ? "pending" : "full";

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

const minuteRule = (contract: ContractCoverage): Rule => ({
  table: Table.MINUTES,
  refetch: false,
  exchanges: [],
  instruments: [contract.contract],
  firstDay: "",
  lastDay: "",
});

// One acquisition for days to refetch and one for days never asked for.
function merged(rules: Rule[]): Rule[] {
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
export const productRules = (contracts: ContractCoverage[]) =>
  merged(
    contracts
      .filter(item => item.listed || ["gap", "pending"].includes(stateOf(item.daily)))
      .flatMap(item => dailyRule(item) ?? []),
  );

export const covers = (task: AcquisitionTask, rule: Rule) => {
  const asked = task.request!;
  const target = rule.instruments.length
    ? asked.instruments.some(
        item => item === rule.instruments[0] || rule.instruments[0]!.startsWith(`${item}/`),
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
    early: t("自 {day}", { day: series?.firstDay ?? "" }),
    empty: t("来源无数据"),
  })[state];

const summaryText = (state: State, series: SeriesSummary | undefined, minutes: boolean) =>
  state === "none"
    ? t("未采集")
    : state === "gap"
      ? t("缺 {count} 个合约", { count: series!.withGaps })
      : state === "pending"
        ? t("待更新")
        : minutes
          ? t("{count} 个合约", { count: series!.held })
          : t("齐全");

// The time span every bar in the list is drawn against.
function axisOf(rows: readonly ProductRow[]) {
  const held = rows.flatMap(({ coverage }) =>
    [coverage?.daily?.firstDay, coverage?.minutes?.firstDay].filter((day): day is string => !!day),
  );
  const last = beijingDay(-1);
  const first = held.length ? earliest(held) : beijingDay(-366);
  const from = Date.parse(first);
  const span = Math.max(1, Date.parse(last) - from);
  const at = (day: string) => Math.min(1, Math.max(0, (Date.parse(day) - from) / span));
  const [startYear, startMonth] = first.split("-").map(Number);
  const [endYear, endMonth] = last.split("-").map(Number);
  const months = (endYear! - startYear!) * 12 + endMonth! - startMonth!;
  const step = months > 96 ? 24 : months > 36 ? 12 : 6;
  const ticks: { label: string; left: number }[] = [];
  for (let year = startYear!; year <= endYear!; year += 1)
    for (let month = 1; month <= 12; month += step) {
      const day = `${year}-${String(month).padStart(2, "0")}-01`;
      if (day >= first && day <= last && at(day) < 0.94)
        ticks.push({ label: step >= 12 ? String(year) : day.slice(0, 7), left: at(day) });
    }
  return { at, ticks };
}
type Axis = ReturnType<typeof axisOf>;

function Bar({ axis, part, from, to }: { axis: Axis; part: string; from: string; to: string }) {
  const left = axis.at(from);
  return (
    <i
      data-part={part}
      style={{
        left: `${(left * 100).toFixed(2)}%`,
        width: `${(Math.max(0, axis.at(to) - left) * 100).toFixed(2)}%`,
      }}
    />
  );
}

function ContractRow({
  contract,
  axis,
  nested,
  running,
  acquires,
  acquiresMinutes,
  start,
  view,
}: {
  contract: ContractCoverage;
  axis: Axis;
  nested: boolean;
  running: readonly AcquisitionTask[];
  acquires: boolean;
  acquiresMinutes: boolean;
  start: (rules: Rule[]) => void;
  view: (contract: string) => void;
}) {
  const code = contractCode(contract.contract);
  const daily = contract.daily;
  const dailyState = stateOf(daily);
  const minuteState = stateOf(contract.minutes);
  const rule = acquires ? dailyRule(contract) : undefined;
  const minutes = minuteRule(contract);
  const minuteBusy = running.some(task => covers(task, minutes));
  const busy = !!rule && running.some(task => covers(task, rule));
  const through = throughOf(contract);
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
        <div className="coverage-timeline" aria-hidden="true">
          {contract.listDay && <Bar axis={axis} part="life" from={contract.listDay} to={through} />}
          {daily?.days ? (
            <Bar axis={axis} part="held" from={daily.firstDay} to={daily.lastDay} />
          ) : null}
          {daily?.pendingDays ? (
            <Bar axis={axis} part="pending" from={daily.lastDay} to={through} />
          ) : null}
          {daily?.gaps.map(gap => (
            <Bar key={gap.first} axis={axis} part="gap" from={gap.first} to={gap.last} />
          ))}
        </div>
      </td>
      <td>
        <StateText state={dailyState}>{seriesText(dailyState, daily)}</StateText>
      </td>
      <td>
        {acquiresMinutes && minuteState !== "full" && minuteState !== "empty" ? (
          <button
            type="button"
            className="coverage-link"
            disabled={minuteBusy}
            aria-label={t("采集 {code} 分钟线", { code })}
            onClick={() => start([minutes])}
          >
            {minuteBusy
              ? t("采集中")
              : minuteState === "none"
                ? t("采集")
                : seriesText(minuteState, contract.minutes)}
          </button>
        ) : (
          <StateText state={minuteState}>{seriesText(minuteState, contract.minutes)}</StateText>
        )}
      </td>
      <td>
        {busy ? (
          <span className="coverage-busy">{t("采集中")}</span>
        ) : rule ? (
          <button
            type="button"
            className="coverage-action"
            aria-label={t("补全 {name}", { name: code })}
            onClick={() => start([rule])}
          >
            {t("补全")}
          </button>
        ) : null}
      </td>
    </tr>
  );
}

type Confirm = { title: string; from: string; exchange: string; rule: Rule };

// Products with their contracts: what is held of each, from listing to delisting
// or the latest day, and one action that takes what is missing.
export function CoverageList({
  rows,
  contracts,
  open,
  setOpen,
  query,
  running,
  acquires,
  acquiresMinutes,
  start,
  collect,
  view,
}: {
  rows: readonly ProductRow[];
  // Those of the open product, once read.
  contracts: readonly ContractCoverage[] | undefined;
  open: string;
  setOpen: (product: string) => void;
  query: string;
  running: readonly AcquisitionTask[];
  acquires: boolean;
  acquiresMinutes: boolean;
  start: (rules: Rule[]) => void;
  collect: (row: ProductRow) => void;
  view: (contract: string) => void;
}) {
  const [historyOpen, setHistoryOpen] = useState("");
  const [yearOpen, setYearOpen] = useState("");
  const [confirm, setConfirm] = useState<Confirm>();
  const axis = axisOf(rows);
  const rank = (row: ProductRow) => {
    const state = summaryState(row.coverage?.daily);
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
  const rowProps = { axis, running, acquires, acquiresMinutes, start, view };

  const askHistory = (product: string, history: ContractCoverage[], from: string, to: string) => {
    const chosen = history.filter(item => {
      const year = deliveryYear(item.contract);
      return year >= from && year <= to && !!dailyRule(item);
    });
    const rules = chosen.flatMap(item => dailyRule(item) ?? []);
    if (!rules.length) return;
    const years = chosen.map(item => deliveryYear(item.contract));
    const [low, high] = [earliest(years), latest(years)];
    const firstDay = earliest(rules.map(rule => rule.firstDay));
    setConfirm({
      title: t("采集 {code} {years} 年交割的 {count} 个合约", {
        code: product.split("/")[1]!,
        years: low === high ? low : `${low}–${high}`,
        count: chosen.length,
      }),
      from: firstDay.slice(0, 7),
      exchange: product.split("/")[0]!,
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
          <col className="coverage-col-state" />
          <col className="coverage-col-action" />
        </colgroup>
        <thead>
          <tr>
            <th scope="col">{t("品种")}</th>
            <th scope="col">{t("在市 / 全部")}</th>
            <th scope="col">
              <div className="coverage-axis">
                {axis.ticks.map(tick => (
                  <span key={tick.label} style={{ left: `${(tick.left * 100).toFixed(2)}%` }}>
                    {tick.label}
                  </span>
                ))}
              </div>
            </th>
            <th scope="col">{t("日线")}</th>
            <th scope="col">{t("分钟线")}</th>
            <th scope="col" aria-label={t("操作")} />
          </tr>
        </thead>
        {ordered.map(row => {
          const expanded = open === row.id;
          const daily = row.coverage?.daily;
          const minutes = row.coverage?.minutes;
          const dailyState = summaryState(daily);
          const minuteState = summaryState(minutes);
          const busy = running.some(
            task =>
              task.request!.table === Table.DAILY && task.request!.exchanges.includes(row.exchange),
          );
          const items = expanded ? (contracts ?? []) : [];
          const listed = items.filter(item => item.listed && matches(item));
          // A historical contract with a gap stays in view; the rest fold by delivery year.
          const history = items.filter(item => !item.listed);
          const flagged = history.filter(item => stateOf(item.daily) === "gap" && matches(item));
          const years = [...new Set(history.map(item => deliveryYear(item.contract)))].sort(
            (a, b) => b.localeCompare(a),
          );
          const historyHeld = history.filter(item => item.daily?.days).length;
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
                  <div className="coverage-timeline" aria-hidden="true">
                    {daily?.held ? (
                      <Bar axis={axis} part="held" from={daily.firstDay} to={daily.lastDay} />
                    ) : null}
                    {daily?.gaps.map(gap => (
                      <Bar key={gap.first} axis={axis} part="gap" from={gap.first} to={gap.last} />
                    ))}
                  </div>
                </td>
                <td>
                  <StateText state={dailyState}>{summaryText(dailyState, daily, false)}</StateText>
                </td>
                <td>
                  <StateText state={minuteState}>
                    {summaryText(minuteState, minutes, true)}
                  </StateText>
                </td>
                <td>
                  {!acquires ? null : busy ? (
                    <span className="coverage-busy">{t("采集中")}</span>
                  ) : dailyState === "gap" || dailyState === "pending" || dailyState === "none" ? (
                    <button
                      type="button"
                      className="coverage-action"
                      aria-label={t(dailyState === "none" ? "采集 {name}" : "补全 {name}", {
                        name: row.id,
                      })}
                      onClick={() => collect(row)}
                    >
                      {dailyState === "none" ? t("采集") : t("补全")}
                    </button>
                  ) : null}
                </td>
              </tr>
              {expanded && !contracts && (
                <tr data-kind="note">
                  <td colSpan={6}>{t("读取中")}</td>
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
                  <td className="coverage-note">{t("已采 {count}", { count: historyHeld })}</td>
                  <td colSpan={3} />
                </tr>
              )}
              {showHistory &&
                years
                  .filter(year => !searchedYears || searchedYears.has(year))
                  .map(year => {
                    const inYear = history.filter(item => deliveryYear(item.contract) === year);
                    const held = inYear.filter(item => item.daily?.days).length;
                    const lacking = inYear.filter(item => !!dailyRule(item)).length;
                    const withGaps = inYear.filter(item => stateOf(item.daily) === "gap").length;
                    const yearKey = `${row.id}/${year}`;
                    const shown = yearOpen === yearKey || !!searchedYears;
                    const yearRule = inYear.flatMap(item => dailyRule(item) ?? [])[0];
                    const yearBusy = !!yearRule && running.some(task => covers(task, yearRule));
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
                          {held} / {inYear.length}
                        </td>
                        <td />
                        <td>
                          {!held ? (
                            <StateText state="none">{t("未采集")}</StateText>
                          ) : withGaps ? (
                            <StateText state="gap">
                              {t("缺 {count} 个", { count: withGaps })}
                            </StateText>
                          ) : lacking ? (
                            <StateText state="early">
                              {t("{count} 个未采全", { count: lacking })}
                            </StateText>
                          ) : (
                            <StateText state="full">{t("齐全")}</StateText>
                          )}
                        </td>
                        <td />
                        <td>
                          {!acquires || !lacking ? null : yearBusy ? (
                            <span className="coverage-busy">{t("采集中")}</span>
                          ) : (
                            <button
                              type="button"
                              className="coverage-action"
                              aria-label={t(held ? "补全 {name}" : "采到 {name}", {
                                name: `${row.id} ${year}`,
                              })}
                              onClick={() =>
                                askHistory(row.id, history, year, held ? year : newest)
                              }
                            >
                              {held ? t("补全") : t("采到这年")}
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
          <p className="subtle">
            {t("日线按交易所整日采集，{exchange} 其他品种这些交易日的数据也会入库", {
              exchange: confirm.exchange,
            })}
          </p>
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
