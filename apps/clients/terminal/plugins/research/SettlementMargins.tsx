import type { BacktestResult, SettlementMargin, HistoricalMargins } from "../../src/bridge/client";
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
export function SettlementMarginDetails({
  events = [],
  historical,
}: {
  events?: SettlementMargin[];
  historical?: HistoricalMargins;
}) {
  if (historical) return <HistoricalMarginDetails value={historical} />;
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

export function HistoricalMarginDetails({ value }: { value: HistoricalMargins }) {
  return (
    <section aria-label={t("历史保证金")}>
      <h4>{t("历史保证金")}</h4>
      <p>
        {t(
          "上期所交易所投机保证金，比例为小数。仅替换日初和结算保证金率，手续费和合约规格仍使用已确认的参数。",
        )}
      </p>
      <p className="subtle">
        {t("这是当前可取得的历史报表，不证明历史盘中首次公布时间，也不包含期货公司加收。")}
      </p>
      <p>
        {value.contract} · {value.source}
      </p>
      <table aria-label={t("历史保证金参数")}>
        <thead>
          <tr>
            {["交易日", "日初保证金比例", "结算保证金比例"].map(label => (
              <th key={label}>{t(label)}</th>
            ))}
          </tr>
        </thead>
        <tbody>
          {value.days.map(day => (
            <tr key={day.trading_day}>
              <td>{day.trading_day}</td>
              <td>{day.day_start_rate}</td>
              <td>{day.settlement_rate}</td>
            </tr>
          ))}
        </tbody>
      </table>
      <details>
        <summary>{t("保证金来源证据")}</summary>
        <p>
          {t("固定快照")} <code>{value.snapshot}</code>
        </p>
        <p>
          {t("单位依据")} {value.unit_reference}
        </p>
        <p>
          {t("阶段依据")} {value.phase_reference}
        </p>
        <p>
          {t("适用基础依据")} {value.basis_reference}
        </p>
        {value.days.map(day => (
          <div key={day.trading_day}>
            <strong>{day.trading_day}</strong>
            <p>
              {day.day_start_url} · {day.day_start_updated} · SHA-256 {day.day_start_sha256}
            </p>
            <p>
              {day.settlement_url} · {day.settlement_updated} · SHA-256 {day.settlement_sha256}
            </p>
            <p>
              {t("分区修订")} {day.revision_snapshot}
            </p>
            <p>
              {t("内容摘要")} {day.content} · {t("对象摘要")} {day.object}
            </p>
            <p>
              {t("来源实现摘要")} {day.provider_artifact} · {t("取得时间纳秒")} {day.acquired_at_ns}
            </p>
          </div>
        ))}
      </details>
    </section>
  );
}
