import { useState } from "react";
import { translate, type MarketCoverage, type MessageValues } from "../contract";
import { productOf } from "./ContractPicker";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);

// Trading days with minutes, per contract of one product and month.
// With `onFill`, every cell offers to acquire that contract's month.
export function MinuteCoverage({
  contracts,
  onFill,
}: {
  contracts: MarketCoverage["contracts"];
  onFill?: (contract: string, month: string) => void;
}) {
  const products = [...new Set(contracts.map(item => productOf(item.contract)))].sort();
  const [chosen, setChosen] = useState("");
  const product = products.includes(chosen) ? chosen : (products[0] ?? "");
  const rows = contracts.filter(item => productOf(item.contract) === product);
  const months = [...new Set(rows.flatMap(item => item.months.map(m => m.month)))].sort();
  return (
    <>
      {rows.length ? (
        <>
          <label>
            {t("品种")}
            <select value={product} onChange={event => setChosen(event.target.value)}>
              {products.map(item => (
                <option key={item}>{item}</option>
              ))}
            </select>
          </label>
          <div className="coverage-matrix">
            <table className="data-table" aria-label={t("分钟线覆盖")}>
              <thead>
                <tr>
                  <th>{t("合约")}</th>
                  {months.map(month => (
                    <th key={month}>{month}</th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {rows.map(item => {
                  const byMonth = new Map(item.months.map(m => [m.month, m]));
                  return (
                    <tr key={item.contract}>
                      <th scope="row">{item.contract}</th>
                      {months.map(month => {
                        const cell = byMonth.get(month);
                        return (
                          <td
                            key={month}
                            title={cell ? t("{days} 天", { days: cell.days }) : t("无数据")}
                          >
                            {onFill ? (
                              <button
                                type="button"
                                aria-label={t("补采 {target} {month}", {
                                  target: item.contract,
                                  month,
                                })}
                                onClick={() => onFill(item.contract, month)}
                              >
                                {cell?.days ?? "+"}
                              </button>
                            ) : (
                              (cell?.days ?? "")
                            )}
                          </td>
                        );
                      })}
                    </tr>
                  );
                })}
              </tbody>
            </table>
          </div>
        </>
      ) : (
        <p className="subtle">{t("仓库里还没有分钟线。")}</p>
      )}
    </>
  );
}
