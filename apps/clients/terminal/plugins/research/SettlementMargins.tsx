import type { BacktestResult, SettlementMargin } from "../../src/bridge/client";
import { t } from "./shared";

const scope =
  "仅支持单合约日线、单策略全样本回测；结算当日的每手保证金须为零。次一交易日的费用必须在费用日程中显式填写。";
const assumption =
  "这是用户填写的模型假设，不代表已核验的交易所或期货公司历史保证金。比例按小数填写，必须大于 0 且小于 1。";

export function SettlementMarginEditor({
  events,
  supported,
  onChange,
}: {
  events: SettlementMargin[];
  supported: boolean;
  onChange: (events: SettlementMargin[]) => void;
}) {
  return (
    <section aria-label={t("结算保证金调整")}>
      <h4>{t("结算保证金调整")}</h4>
      <p className="subtle">{t(scope)}</p>
      <p className="subtle">{t(assumption)}</p>
      {events.map((event, index) => (
        <div className="research-fields" key={index}>
          {(
            [
              ["trading_day", "调整交易日"],
              ["rate", "结算保证金比例"],
              ["source", "模型依据说明"],
            ] as const
          ).map(([key, label]) => (
            <label key={key}>
              {t(label)}
              <input
                type={key === "trading_day" ? "date" : "text"}
                inputMode={key === "rate" ? "decimal" : undefined}
                required
                value={event[key]}
                onChange={e =>
                  onChange(
                    events.map((row, i) => (i === index ? { ...row, [key]: e.target.value } : row)),
                  )
                }
              />
            </label>
          ))}
          <button type="button" onClick={() => onChange(events.filter((_, i) => i !== index))}>
            {t("删除调整")}
          </button>
        </div>
      ))}
      <button
        type="button"
        disabled={!supported}
        onClick={() => onChange([...events, { trading_day: "", rate: "", source: "" }])}
      >
        {t("添加结算调整")}
      </button>
      {!supported && events.length > 0 && (
        <p role="alert">{t("当前配置不支持结算保证金调整，请恢复支持的配置或删除调整。")}</p>
      )}
    </section>
  );
}

// The same fixed assumptions are shown before submission and on reopening a task.
export function SettlementMarginDetails({ events = [] }: { events?: SettlementMargin[] }) {
  if (!events.length) return null;
  return (
    <section aria-label={t("结算保证金参数")}>
      <h4>{t("结算保证金参数")}</h4>
      <p className="subtle">{t(assumption)}</p>
      <div className="research-table">
        <table aria-label={t("结算保证金参数")}>
          <thead>
            <tr>
              {["调整交易日", "结算保证金比例", "模型依据说明"].map(label => (
                <th key={label}>{t(label)}</th>
              ))}
            </tr>
          </thead>
          <tbody>
            {events.map((event, index) => (
              <tr key={index}>
                <td>{event.trading_day}</td>
                <td>{event.rate}</td>
                <td>{event.source}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </section>
  );
}

export function SettlementMarginResults({
  settlements,
}: {
  settlements: BacktestResult["settlements"];
}) {
  const adjusted = settlements.filter(day => day.margin_revaluation);
  if (!adjusted.length) return null;
  return (
    <details className="research-settlements">
      <summary>{t("结算保证金影响")}</summary>
      <p className="subtle">
        {t(
          "先结算盈亏，再调整保证金占用；调整不收取手续费，不改变持仓，也不自动强平。以下金额均为盈亏结算后、保证金调整前后的记录。",
        )}
      </p>
      <div className="research-table">
        <table aria-label={t("结算保证金影响")}>
          <thead>
            <tr>
              {[
                "交易日",
                "调整前比例",
                "调整后比例",
                "调整前保证金",
                "调整后保证金",
                "调整前可用资金",
                "调整后可用资金",
              ].map(label => (
                <th key={label}>{t(label)}</th>
              ))}
            </tr>
          </thead>
          <tbody>
            {adjusted.map(day => {
              const change = day.margin_revaluation!;
              return (
                <tr key={day.trading_day}>
                  <td>{day.trading_day}</td>
                  <td>{change.previous_rate}</td>
                  <td>{change.rate}</td>
                  <td>{change.margin_before}</td>
                  <td>{day.occupancy!.margin}</td>
                  <td>{change.available_before}</td>
                  <td>{day.occupancy!.available}</td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>
    </details>
  );
}
