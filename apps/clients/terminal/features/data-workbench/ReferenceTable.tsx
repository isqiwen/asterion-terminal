import { useEffect, useState } from "react";
import { Table, type TableRows } from "../../src/generated/asterion/warehouse_pb";
import { warehouseData } from "../../src/bridge/warehouse-data";
import { type MessageValues, translate } from "../../src/i18n";
import { type DisplayError, ErrorNotice, asDisplayError } from "../../src/i18n/errors";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);
const page = 100;
const today = () => new Date().toISOString().slice(0, 10);

// Browse one table of one exchange in a fixed version, limited to the selected dates.
export function ReferenceTable({
  version,
  table,
  name,
  exchanges,
}: {
  version: string;
  table: Table;
  name: string;
  exchanges: string[];
}) {
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
    <>
      <div className="data-fields">
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
            <table className="data-table" aria-label={name}>
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
    </>
  );
}
