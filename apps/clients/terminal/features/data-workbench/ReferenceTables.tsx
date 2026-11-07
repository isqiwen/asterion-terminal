import { useEffect, useState } from "react";
import { Table, type TableRows } from "../../src/generated/asterion/warehouse_pb";
import {
  ErrorNotice,
  asDisplayError,
  warehouseData,
  translate,
  type DisplayError,
  type MessageValues,
} from "../contract";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);
const names: Partial<Record<Table, string>> = {
  [Table.CONTRACTS]: "合约表",
  [Table.CALENDAR]: "交易日历",
  [Table.SETTLEMENT]: "结算参数",
  [Table.PRICE_TICKS]: "历史报价单位",
};
export const referenceTables = [
  Table.CONTRACTS,
  Table.CALENDAR,
  Table.SETTLEMENT,
  Table.PRICE_TICKS,
];
const page = 100;
const today = () => new Date().toISOString().slice(0, 10);

// Browse one exchange in a fixed version, limited to the selected dates.
export function ReferenceTables({
  version,
  tables,
  exchanges,
}: {
  version: string;
  tables: Table[];
  exchanges: string[];
}) {
  const [table, setTable] = useState(tables[0]!);
  const [exchange, setExchange] = useState(exchanges[0] ?? "");
  const [first, setFirst] = useState(() => `${today().slice(0, 4)}-01-01`);
  const [last, setLast] = useState(today);
  const [offset, setOffset] = useState(0);
  const [rows, setRows] = useState<TableRows>();
  const [error, setError] = useState<DisplayError>("");
  useEffect(() => {
    let active = true;
    void warehouseData({
      case: "getTableRows",
      value: {
        dataset: { version },
        table,
        exchange,
        firstDay: table === Table.CONTRACTS ? "" : first,
        lastDay: table === Table.CONTRACTS ? "" : last,
        offset,
      },
    })
      .then(result => {
        if (!active) return;
        setRows(result);
        setError("");
      })
      .catch(reason => active && setError(asDisplayError(reason)));
    return () => {
      active = false;
    };
  }, [version, table, exchange, first, last, offset]);
  // Any change of what is shown starts from its first page.
  const choose = (apply: () => void) => {
    apply();
    setOffset(0);
  };
  return (
    <section className="data-card" aria-label={t("参考表")}>
      <header>
        <h3>{t("参考表")}</h3>
      </header>
      <div className="data-fields">
        <label>
          {t("表")}
          <select
            value={table}
            onChange={event => choose(() => setTable(Number(event.target.value)))}
          >
            {tables.map(item => (
              <option key={item} value={item}>
                {t(names[item]!)}
              </option>
            ))}
          </select>
        </label>
        <label>
          {t("交易所")}
          <select
            value={exchange}
            onChange={event => choose(() => setExchange(event.target.value))}
          >
            {exchanges.map(item => (
              <option key={item}>{item}</option>
            ))}
          </select>
        </label>
        {table !== Table.CONTRACTS && (
          <>
            <label>
              {t("开始日期")}
              <input
                type="date"
                value={first}
                onChange={event => choose(() => setFirst(event.target.value))}
              />
            </label>
            <label>
              {t("结束日期")}
              <input
                type="date"
                value={last}
                onChange={event => choose(() => setLast(event.target.value))}
              />
            </label>
          </>
        )}
      </div>
      <ErrorNotice error={error} />
      {rows?.rows.length ? (
        <>
          <div className="coverage-matrix">
            <table className="data-table" aria-label={t(names[table]!)}>
              <thead>
                <tr>
                  {rows.columns.map(column => (
                    <th key={column}>{column}</th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {rows.rows.map((row, index) => (
                  <tr key={offset + index}>
                    {row.values.map((value, column) => (
                      <td key={column}>{value}</td>
                    ))}
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
          <div className="data-actions">
            <button type="button" disabled={!offset} onClick={() => setOffset(offset - page)}>
              {t("上一页")}
            </button>
            <span className="subtle">
              {t("第 {first}–{last} 行，共 {total} 行", {
                first: offset + 1,
                last: offset + rows.rows.length,
                total: rows.total,
              })}
            </span>
            <button
              type="button"
              disabled={offset + rows.rows.length >= rows.total}
              onClick={() => setOffset(offset + page)}
            >
              {t("下一页")}
            </button>
          </div>
        </>
      ) : (
        !error && <p className="subtle">{t("所选范围内没有数据。")}</p>
      )}
    </section>
  );
}
