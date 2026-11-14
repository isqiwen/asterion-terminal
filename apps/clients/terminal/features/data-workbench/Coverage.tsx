import { useMemo, useState } from "react";
import { warehouseData } from "../../src/bridge/warehouse-data";
import { Table, type DatasetCoverage } from "../../src/generated/asterion/warehouse_pb";
import type { TerminalContext } from "../../src/host/feature-registry";
import { useMarket } from "../../src/host/market";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { ErrorNotice } from "../../src/i18n/errors";
import { Acquisition, type Preset } from "./Acquisition";
import { VersionBanner, WarehousePage } from "./Page";
import { exchanges, t, tableName, usePolled } from "./dataset";

const day = (offset: number) => new Date(Date.now() + offset * 86400000).toISOString().slice(0, 10);
// A month's days up to yesterday: providers only hold finished trading days.
const monthRange = (month: string) => {
  const [year, number] = month.split("-").map(Number);
  const last = new Date(Date.UTC(year!, number!, 0)).toISOString().slice(0, 10);
  return { first: `${month}-01`, last: last < day(-1) ? last : day(-1) };
};
const monthsOf = (year: string) =>
  Array.from({ length: 12 }, (_, index) => `${year}-${String(index + 1).padStart(2, "0")}`);

// One row of the matrix: a product's daily bars or a contract's minutes. Open
// days count through yesterday and from the row's first day.
type Month = { days: number; open: number; empty: number; pending: number };
type Row = { id: string; exchange: string; months: Map<string, Month> };
// "partial" has gaps, "missing" is a whole month without bars, "pending" waits
// only for an update, "empty" lacks only what every source answered empty,
// "held" has no calendar; nothing outside the row's span.
type Cell = "full" | "partial" | "missing" | "pending" | "empty" | "held" | "";

const gaps = (cell: Month) => Math.max(0, cell.open - cell.days - cell.empty - cell.pending);

function cellOf(row: Row, month: string, first: string, last: string): Cell {
  const cell = row.months.get(month);
  if (!cell) return month > first && month < last ? "missing" : "";
  if (!cell.open) return "held";
  if (gaps(cell)) return cell.days || cell.pending ? "partial" : "missing";
  if (cell.pending) return "pending";
  return cell.empty ? "empty" : "full";
}

function rowsOf(coverage: DatasetCoverage, minutes: boolean): Row[] {
  const month = (m: {
    month: string;
    days: number;
    openDays: number;
    emptyDays: number;
    pendingDays: number;
  }) =>
    [
      m.month,
      { days: m.days, open: m.openDays, empty: m.emptyDays, pending: m.pendingDays },
    ] as const;
  return minutes
    ? coverage.contracts.map(item => ({
        id: item.contract,
        exchange: item.contract.split("/")[0]!,
        months: new Map(item.months.map(month)),
      }))
    : coverage.products.map(item => ({
        id: item.product,
        exchange: item.product.split("/")[0]!,
        months: new Map(item.months.map(month)),
      }));
}

// What a cell holds, as its tooltip says it.
function describe(cell: Month) {
  if (!cell.open) return t("{days} 天", { days: cell.days });
  return [
    `${cell.days} / ${cell.open}`,
    gaps(cell) && t("缺 {days} 天", { days: gaps(cell) }),
    cell.pending && t("{days} 天待更新", { days: cell.pending }),
    cell.empty && t("{days} 天来源无数据", { days: cell.empty }),
  ]
    .filter(Boolean)
    .join(" · ");
}

// Which days each product (daily) or contract (minutes) holds, a year at a
// time, and acquisition of what is missing.
export function Coverage(context: TerminalContext) {
  return (
    <WarehousePage context={context} label={t("数据覆盖")}>
      {() => <CoverageBody context={context} />}
    </WarehousePage>
  );
}

function CoverageBody({ context }: { context: TerminalContext }) {
  // A run's record or the activity opens an earlier version: param `version`.
  const [viewed, setViewed] = useState(context.workspaceParams?.version ?? "");
  const [kind, setKind] = useWorkspaceDraft<"daily" | "minutes">("coverage-kind", "daily");
  const [exchange, setExchange] = useWorkspaceDraft("coverage-exchange", "");
  const [search, setSearch] = useWorkspaceDraft("coverage-search", "");
  const [chosenYear, setYear] = useState("");
  const [acquiring, setAcquiring] = useState<Preset>();
  const catalog = useMarket()?.board.catalog?.contracts;
  const options = usePolled(() => warehouseData({ case: "getAcquisitionOptions", value: {} }), []);
  const state = usePolled(async () => {
    const current = await warehouseData({ case: "getDatasetSummary", value: {} });
    const latest = current.dataset?.version ?? "";
    const version = viewed || latest;
    const coverage = version
      ? await warehouseData({ case: "getDatasetCoverage", value: { version } })
      : undefined;
    return { latest, coverage };
  }, [viewed]);
  const minutes = kind === "minutes";
  const table = minutes ? Table.MINUTES : Table.DAILY;
  const coverage = state.value?.coverage;
  const rows = useMemo(() => (coverage ? rowsOf(coverage, minutes) : []), [coverage, minutes]);
  const present = exchanges.filter(item => rows.some(row => row.exchange === item));
  const query = search.trim().toUpperCase();
  const shown = rows.filter(
    row =>
      (!exchange || row.exchange === exchange) && (!query || row.id.toUpperCase().includes(query)),
  );
  const years = [
    ...new Set(rows.flatMap(row => [...row.months.keys()].map(month => month.slice(0, 4)))),
  ]
    .sort()
    .reverse();
  const year = years.includes(chosenYear) ? chosenYear : (years[0] ?? "");
  const known = useMemo(
    () => [
      ...new Set([
        ...(catalog ?? []).map(item => item.contract),
        ...(coverage?.contracts ?? []).map(item => item.contract),
      ]),
    ],
    [catalog, coverage],
  );
  const acquires = !viewed && (options.value?.tables.includes(table) ?? false);
  return (
    <>
      <VersionBanner
        viewed={viewed}
        latest={state.value?.latest ?? ""}
        back={() => setViewed("")}
      />
      <div className="data-coverage">
        <ErrorNotice error={state.error || options.error} />
        <div className="data-toolbar">
          <div className="tabs" role="group" aria-label={t("数据")}>
            {(["daily", "minutes"] as const).map(item => (
              <button
                key={item}
                type="button"
                aria-pressed={kind === item}
                onClick={() => setKind(item)}
              >
                {tableName(item === "minutes" ? Table.MINUTES : Table.DAILY)}
              </button>
            ))}
          </div>
          <select
            aria-label={t("交易所")}
            value={exchange}
            onChange={event => setExchange(event.target.value)}
          >
            <option value="">{t("全部交易所")}</option>
            {present.map(item => (
              <option key={item}>{item}</option>
            ))}
          </select>
          <input
            type="search"
            aria-label={minutes ? t("搜索合约") : t("搜索品种")}
            placeholder={minutes ? t("搜索合约") : t("搜索品种")}
            value={search}
            onChange={event => setSearch(event.target.value)}
          />
          {years.length > 0 && (
            <select
              aria-label={t("年份")}
              value={year}
              onChange={event => setYear(event.target.value)}
            >
              {years.map(item => (
                <option key={item}>{item}</option>
              ))}
            </select>
          )}
          <span className="data-legend" aria-hidden="true">
            <i data-cell="full" /> {t("齐全")} <i data-cell="partial" /> {t("有缺口")}{" "}
            <i data-cell="missing" /> {t("整月缺失")} <i data-cell="pending" /> {t("待更新")}{" "}
            <i data-cell="empty" /> {t("来源无数据")}
          </span>
          {acquires && (
            <button type="button" className="primary" onClick={() => setAcquiring({})}>
              {t("采集")}
            </button>
          )}
        </div>
        {!state.value ? (
          <p className="subtle" role="status">
            {t("读取中")}
          </p>
        ) : !rows.length ? (
          <p className="subtle">{t(minutes ? "还没有分钟线" : "还没有日线")}</p>
        ) : (
          <Matrix
            rows={shown}
            months={monthsOf(year)}
            label={minutes ? t("分钟线覆盖") : t("日线覆盖")}
            heading={minutes ? t("合约") : t("品种")}
            onFill={
              acquires
                ? (id, month) => setAcquiring({ instruments: [id], ...monthRange(month) })
                : undefined
            }
          />
        )}
      </div>
      {acquiring && options.value && (
        <Acquisition
          table={table}
          name={tableName(table)}
          options={options.value}
          known={known}
          preset={acquiring}
          close={() => setAcquiring(undefined)}
          started={() => {
            setAcquiring(undefined);
            void state.refresh();
          }}
        />
      )}
    </>
  );
}

function Matrix({
  rows,
  months,
  label,
  heading,
  onFill,
}: {
  rows: Row[];
  months: string[];
  label: string;
  heading: string;
  onFill?: (id: string, month: string) => void;
}) {
  const groups = exchanges
    .map(exchange => ({ exchange, rows: rows.filter(row => row.exchange === exchange) }))
    .filter(group => group.rows.length);
  return (
    <div className="coverage-matrix">
      <table className="data-table" aria-label={label}>
        <thead>
          <tr>
            <th>{heading}</th>
            {months.map(month => (
              <th key={month}>{month.slice(5)}</th>
            ))}
          </tr>
        </thead>
        {groups.map(group => (
          <tbody key={group.exchange}>
            <tr className="coverage-group">
              <th colSpan={months.length + 1}>{group.exchange}</th>
            </tr>
            {group.rows.map(row => {
              const held = [...row.months.keys()].sort();
              return (
                <tr key={row.id}>
                  <th scope="row">{row.id}</th>
                  {months.map(month => {
                    const cell = row.months.get(month);
                    const state = cellOf(row, month, held[0]!, held.at(-1)!);
                    return (
                      <td
                        key={month}
                        data-cell={state || undefined}
                        title={cell ? describe(cell) : state && t("无数据")}
                      >
                        {onFill && ["partial", "missing", "pending"].includes(state) ? (
                          <button
                            type="button"
                            aria-label={t("补采 {target} {month}", { target: row.id, month })}
                            onClick={() => onFill(row.id, month)}
                          >
                            {cell?.days ?? "+"}
                          </button>
                        ) : (
                          cell?.days
                        )}
                      </td>
                    );
                  })}
                </tr>
              );
            })}
          </tbody>
        ))}
      </table>
    </div>
  );
}
