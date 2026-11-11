import type { DatasetCoverage } from "../../src/generated/asterion/warehouse_pb";
import { type MessageValues, translate } from "../../src/i18n";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);

// Days with daily bars per product and month, against the calendar's open days.
// With `onFill`, every incomplete cell offers to acquire that month.
export function DailyCoverage({
  products,
  onFill,
}: {
  products: DatasetCoverage["products"];
  onFill?: (product: string, month: string) => void;
}) {
  const months = [...new Set(products.flatMap(item => item.months.map(m => m.month)))].sort();
  if (!products.length) return <p className="subtle">{t("仓库里还没有日线。")}</p>;
  return (
    <div className="coverage-matrix">
      <table className="data-table" aria-label={t("日线覆盖")}>
        <thead>
          <tr>
            <th>{t("品种")}</th>
            {months.map(month => (
              <th key={month}>{month}</th>
            ))}
          </tr>
        </thead>
        <tbody>
          {products.map(product => {
            const byMonth = new Map(product.months.map(m => [m.month, m]));
            return (
              <tr key={product.product}>
                <th scope="row">{product.product}</th>
                {months.map(month => {
                  const cell = byMonth.get(month);
                  const share = cell?.openDays ? cell.days / cell.openDays : 0;
                  return (
                    <td
                      key={month}
                      data-share={cell ? (share >= 1 ? "full" : "partial") : "none"}
                      title={cell ? `${cell.days} / ${cell.openDays || "?"}` : t("无数据")}
                    >
                      {onFill && share < 1 ? (
                        <button
                          type="button"
                          aria-label={t("补采 {target} {month}", {
                            target: product.product,
                            month,
                          })}
                          onClick={() => onFill(product.product, month)}
                        >
                          {cell ? cell.days : "+"}
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
  );
}
