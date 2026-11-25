import { type ReactNode, useEffect, useState } from "react";
import { Table, type TableRows } from "../../src/generated/asterion/warehouse_pb";
import { warehouseData } from "../../src/bridge/warehouse-data";
import { useErrorNotification } from "../../src/host/notifications";
import { type MessageValues, translate } from "../../src/i18n";
import { type DisplayError, asDisplayError } from "../../src/i18n/errors";
import { resolveProductCode, warehouseName } from "../../src/ui/contract-names";
import { DateInput } from "../../src/ui/DateInput";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);
const page = 100;
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
  warehouse_type: "仓库类型",
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
};

const numericColumns = new Set([
  "multiplier",
  "per_unit",
  "long_margin_rate",
  "short_margin_rate",
  "fee_rate",
  "fee_per_lot",
  "close_today_fee_rate",
  "close_today_fee_per_lot",
  "price_tick",
  "previous_volume",
  "volume",
  "volume_change",
  "forecast_volume",
  "premium",
  "long_position",
  "long_change",
  "short_position",
  "short_change",
  "previous_total",
  "previous_receipts",
  "total",
  "receipts",
  "total_change",
  "receipts_change",
  "previous_capacity",
  "capacity",
  "capacity_change",
]);

const changeColumns = new Set([
  "volume_change",
  "long_change",
  "short_change",
  "total_change",
  "receipts_change",
  "capacity_change",
]);

const timestamp = (value: string) =>
  value
    ? new Date(Number(value)).toLocaleString("sv-SE", { timeZone: "Asia/Shanghai", hour12: false })
    : "—";

const formatNumber = (value: string) => {
  if (!/^-?\d+\.\d+$/.test(value)) return value;
  const trimmed = value.replace(/(\.\d*?[1-9])0+$|\.0+$/, "$1");
  return trimmed === "-0" ? "0" : trimmed;
};

const changeTone = (value: string): "up" | "down" | "flat" | undefined => {
  if (!value) return undefined;
  const parsed = Number(value);
  if (!Number.isFinite(parsed)) return undefined;
  return parsed > 0 ? "up" : parsed < 0 ? "down" : "flat";
};

// Browse one table of one exchange in a fixed version, newest first.
export function ReferenceTable({
  version,
  table,
  name,
  exchanges,
  toolbar,
}: {
  version: string;
  table: Table;
  name: string;
  exchanges: string[];
  toolbar?: ReactNode;
}) {
  const [exchange, setExchange] = useState(exchanges[0] ?? "");
  const [productInput, setProductInput] = useState("");
  const [product, setProduct] = useState("");
  const [day, setDay] = useState("");
  const [offset, setOffset] = useState(0);
  const hasProduct = table !== Table.CALENDAR;
  const hasDay = table !== Table.CONTRACTS;
  useEffect(() => {
    const next = productInput.trim();
    if (next === product) return;
    if (!next) {
      setProduct("");
      setOffset(0);
      return;
    }
    const timer = setTimeout(() => {
      setProduct(next);
      setOffset(0);
    }, 250);
    return () => clearTimeout(timer);
  }, [productInput, product]);
  const resolvedProduct = hasProduct && product ? resolveProductCode(exchange, product) : "";
  const trimmedDay = day.trim();
  const activeDay = hasDay && /^\d{4}-\d{2}-\d{2}$/.test(trimmedDay) ? trimmedDay : "";
  // A followed version replaces the rows in place.
  const query = JSON.stringify([table, exchange, resolvedProduct, activeDay, offset]);
  const [result, setResult] = useState<{ query: string; rows?: TableRows; error: DisplayError }>();
  const rows = result?.query === query ? result.rows : undefined;
  const error = result?.query === query ? result.error : "";
  const shown = (rows?.columns ?? [])
    .map((column, index) => ({ column, index }))
    .filter(
      ({ column, index }) =>
        column !== "observed_ms" &&
        column !== "exchange" &&
        (column === "published_ms" ||
          !rows?.rows.length ||
          rows.rows.some(row => row.values[index] !== "")),
    );
  useEffect(() => {
    let active = true;
    void warehouseData({
      case: "getTableRows",
      value: {
        dataset: { version },
        table,
        exchange,
        offset,
        ...(resolvedProduct ? { product: resolvedProduct } : {}),
        ...(activeDay ? { day: activeDay } : {}),
      },
    })
      .then(rows => {
        if (!active) return;
        // A newer version can have fewer rows than the open page.
        if (offset && !rows.rows.length)
          setOffset(Math.floor(Math.max(rows.total - 1, 0) / page) * page);
        else setResult({ query, rows, error: "" });
      })
      .catch(reason => active && setResult({ query, error: asDisplayError(reason) }));
    return () => {
      active = false;
    };
  }, [query, version, table, exchange, resolvedProduct, activeDay, offset]);
  useErrorNotification(error, "asterion.terminal.data-workbench");
  const filtered = Boolean(product || activeDay);
  const clearFilters = () => {
    setProductInput("");
    setProduct("");
    setDay("");
    setOffset(0);
  };
  return (
    <>
      <div className="data-toolbar">
        {toolbar}
        <div className="reference-filters">
          {hasProduct && (
            <input
              type="search"
              className="reference-search"
              aria-label={t("搜索品种或合约")}
              placeholder={t("品种或合约，如 rb")}
              spellCheck={false}
              autoCorrect="off"
              autoCapitalize="none"
              value={productInput}
              onChange={event => setProductInput(event.target.value)}
              onKeyDown={event => {
                if (event.key !== "Enter") return;
                setProduct(productInput.trim());
                setOffset(0);
              }}
            />
          )}
          {hasDay && (
            <DateInput
              aria-label={t("按日期筛选")}
              value={day}
              onChange={value => {
                setDay(value);
                setOffset(0);
              }}
            />
          )}
          <select
            aria-label={t("交易所")}
            value={exchange}
            onChange={event => {
              setExchange(event.target.value);
              setOffset(0);
            }}
          >
            {exchanges.map(item => (
              <option key={item}>{item}</option>
            ))}
          </select>
        </div>
      </div>
      {rows?.rows.length ? (
        <>
          <div className="reference-table" role="region" aria-label={name} tabIndex={0}>
            <table className="data-table" aria-label={name}>
              <thead>
                <tr>
                  {shown.map(({ column }) => (
                    <th
                      scope="col"
                      key={column}
                      className={numericColumns.has(column) ? "numeric" : undefined}
                    >
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
                    {shown.map(({ column, index: at }) => {
                      const raw = row.values[at] ?? "";
                      if (column === "published_ms") return <td key={column}>{timestamp(raw)}</td>;
                      if (column === "product") {
                        const label = raw ? warehouseName(`${exchange}/${raw}`) : undefined;
                        return (
                          <td key={column}>
                            {raw ? (
                              <span className="reference-product">
                                <code>{raw}</code>
                                {label && (
                                  <>
                                    {" "}
                                    <span>{label}</span>
                                  </>
                                )}
                              </span>
                            ) : (
                              "—"
                            )}
                          </td>
                        );
                      }
                      if (column === "contract")
                        return <td key={column}>{raw ? <code>{raw}</code> : "—"}</td>;
                      if (column === "open")
                        return (
                          <td key={column}>
                            {raw === "true" ? t("是") : raw === "false" ? t("否") : raw || "—"}
                          </td>
                        );
                      if (numericColumns.has(column)) {
                        const formatted = formatNumber(raw);
                        const tone = changeColumns.has(column) ? changeTone(raw) : undefined;
                        return (
                          <td key={column} className="numeric" data-tone={tone}>
                            {!formatted ? "—" : tone === "up" ? `+${formatted}` : formatted}
                          </td>
                        );
                      }
                      return <td key={column}>{raw || "—"}</td>;
                    })}
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
        !error &&
        rows && (
          <div className="reference-empty">
            <span>{t("没有数据")}</span>
            {filtered && (
              <button type="button" onClick={clearFilters}>
                {t("清空筛选")}
              </button>
            )}
          </div>
        )
      )}
    </>
  );
}
