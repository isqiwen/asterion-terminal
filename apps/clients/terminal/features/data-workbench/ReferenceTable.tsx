import { DateInput } from "../../src/ui/DateInput";
import { useEffect, useState } from "react";
import { Table, type TableRows } from "../../src/generated/asterion/warehouse_pb";
import { warehouseData } from "../../src/bridge/warehouse-data";
import { useErrorNotification } from "../../src/host/notifications";
import { type MessageValues, translate } from "../../src/i18n";
import {
  BackendError,
  ErrorNotice,
  type DisplayError,
  asDisplayError,
} from "../../src/i18n/errors";
import { beijingDay } from "./dataset";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);
const page = 100;
const today = () => beijingDay(0);
const columnLabels: Record<string, string> = {
  exchange: "交易所",
  product: "品种",
  contract: "合约",
  delivery_month: "交割月",
  source_instrument: "柜台代码",
  name: "名称",
  day: "交易日",
  open: "是否开市",
  previous_trading_day: "上一交易日",
  list_date: "上市日",
  delist_date: "摘牌日",
  last_delivery_date: "最后交割日",
  multiplier: "合约乘数",
  per_unit: "每手数量",
  trade_unit: "交易单位",
  quote_unit: "报价单位",
  tick_description: "最小变动说明",
  delivery_description: "交割月份说明",
  trading_hours: "交易时间",
  long_margin_rate: "多头保证金率",
  short_margin_rate: "空头保证金率",
  fee_rate: "手续费率",
  fee_per_lot: "每手手续费",
  close_today_fee_rate: "平今手续费率",
  close_today_fee_per_lot: "每手平今手续费",
  effective_from: "生效起日",
  effective_until: "生效止日",
  price_tick: "最小变动价位",
  source_url: "来源链接",
  warehouse: "仓库",
  warehouse_id: "仓库编号",
  area: "地区",
  crop_year: "作物年度",
  grade: "等级",
  brand: "品牌",
  origin: "产地",
  unit: "单位",
  converted: "是否折算",
  previous_volume: "上日仓单数量",
  volume: "成交量",
  volume_change: "数量变化",
  forecast_volume: "有效预报",
  premium: "升贴水",
  member: "会员",
  long_position: "多头持仓",
  long_change: "多头持仓变化",
  short_position: "空头持仓",
  short_change: "空头持仓变化",
  previous_total: "上期库存",
  previous_receipts: "上期仓单",
  total: "库存",
  receipts: "本期仓单",
  total_change: "库存变化",
  receipts_change: "仓单变化",
  previous_capacity: "上期可用库容",
  capacity: "可用库容",
  capacity_change: "库容变化",
  published_ms: "发布时间（北京时间）",
  observed_ms: "采集时间（北京时间）",
};

const timestamp = (value: string) =>
  value
    ? new Date(Number(value)).toLocaleString("sv-SE", { timeZone: "Asia/Shanghai", hour12: false })
    : "—";

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
  const [pagination, setPagination] = useState({ version, offset: 0 });
  const offset = pagination.version === version ? pagination.offset : 0;
  const setOffset = (offset: number) => setPagination({ version, offset });
  const query = JSON.stringify([version, table, exchange, first, last, offset]);
  const [result, setResult] = useState<{ query: string; rows?: TableRows; error: DisplayError }>();
  const rows = result?.query === query ? result.rows : undefined;
  const error = result?.query === query ? result.error : "";
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
      .then(rows => {
        if (!active) return;
        setResult({ query, rows, error: "" });
      })
      .catch(reason => active && setResult({ query, error: asDisplayError(reason) }));
    return () => {
      active = false;
    };
  }, [query, version, table, exchange, first, last, offset]);
  // Any change of what is shown starts from its first page.
  const choose = (apply: () => void) => {
    apply();
    setOffset(0);
  };
  const inputError =
    error instanceof BackendError &&
    error.code === "invalid_request" &&
    error.diagnostic !== "unknown Protobuf field";
  useErrorNotification(!inputError && error ? error : "", "asterion.terminal.data-workbench");
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
              <DateInput value={first} onChange={value => choose(() => setFirst(value))} />
            </label>
            <label>
              {t("结束日期")}
              <DateInput value={last} onChange={value => choose(() => setLast(value))} />
            </label>
          </>
        )}
      </div>
      {inputError && (
        <p className="action-error" role="alert">
          <ErrorNotice error={error} namespace="asterion.terminal.data-workbench" />
        </p>
      )}
      {rows?.rows.length ? (
        <>
          <div className="reference-table" role="region" aria-label={name} tabIndex={0}>
            <table className="data-table" aria-label={name}>
              <thead>
                <tr>
                  {rows.columns.map(column => (
                    <th scope="col" key={column}>
                      {column === "day" && table === Table.INVENTORY
                        ? t("报告日期")
                        : column === "volume" && table === Table.WAREHOUSE_RECEIPTS
                          ? t("仓单数量")
                          : columnLabels[column]
                            ? t(columnLabels[column]!)
                            : column}
                    </th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {rows.rows.map((row, index) => (
                  <tr key={offset + index}>
                    {row.values.map((value, column) => (
                      <td key={column}>
                        {rows.columns[column] === "published_ms" ||
                        rows.columns[column] === "observed_ms"
                          ? timestamp(value)
                          : value}
                      </td>
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
        !error && <p className="subtle">{t("没有数据")}</p>
      )}
    </>
  );
}
