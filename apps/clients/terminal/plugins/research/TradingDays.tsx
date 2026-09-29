import { translate } from "../contract";
const t = (key: string) => translate("asterion.terminal.research", key);
export type TradingDayForm = {
  trading_day: string;
  schedule_source: string;
  settlement_price: string;
  settlement_source: string;
  sessions: { begin: string; end: string }[];
};
export const emptyTradingDay = (): TradingDayForm => ({
  trading_day: "",
  schedule_source: "",
  settlement_price: "",
  settlement_source: "",
  sessions: [{ begin: "", end: "" }],
});
export function TradingDaysEditor({
  days,
  onChange,
}: {
  days: TradingDayForm[];
  onChange: (days: TradingDayForm[]) => void;
}) {
  const update = (index: number, changes: Partial<TradingDayForm>) =>
    onChange(days.map((day, i) => (i === index ? { ...day, ...changes } : day)));
  return (
    <div className="research-days">
      {days.map((day, dayIndex) => (
        <fieldset
          className="research-day"
          key={dayIndex}
          aria-label={`${t("交易日")} ${dayIndex + 1}`}
        >
          <legend>
            {t("交易日")} {dayIndex + 1}
          </legend>
          <label>
            {t("交易日")}
            <input
              aria-label={t("交易日")}
              type="date"
              required
              value={day.trading_day}
              onChange={e => update(dayIndex, { trading_day: e.target.value })}
            />
          </label>
          <fieldset className="research-sessions">
            <legend>{t("交易时段（北京时间）")}</legend>
            {day.sessions.map((row, index) => (
              <div className="research-fields" key={index}>
                <label>
                  {t("开始时刻")}
                  <input
                    aria-label={`${t("开始时刻")} ${index + 1}`}
                    type="datetime-local"
                    step="1"
                    required
                    value={row.begin}
                    onChange={e =>
                      update(dayIndex, {
                        sessions: day.sessions.map((old, i) =>
                          i === index ? { ...old, begin: e.target.value } : old,
                        ),
                      })
                    }
                  />
                </label>
                <label>
                  {t("结束时刻")}
                  <input
                    aria-label={`${t("结束时刻")} ${index + 1}`}
                    type="datetime-local"
                    step="1"
                    required
                    value={row.end}
                    onChange={e =>
                      update(dayIndex, {
                        sessions: day.sessions.map((old, i) =>
                          i === index ? { ...old, end: e.target.value } : old,
                        ),
                      })
                    }
                  />
                </label>
                <button
                  type="button"
                  aria-label={`${t("删除时段")} ${index + 1}`}
                  disabled={day.sessions.length === 1}
                  onClick={() =>
                    update(dayIndex, { sessions: day.sessions.filter((_, i) => i !== index) })
                  }
                >
                  {t("删除时段")}
                </button>
              </div>
            ))}
            <button
              type="button"
              disabled={day.sessions.length >= 16}
              onClick={() =>
                update(dayIndex, { sessions: [...day.sessions, { begin: "", end: "" }] })
              }
            >
              {t("添加时段")}
            </button>
          </fieldset>
          {(
            [
              ["schedule_source", "时段来源"],
              ["settlement_price", "结算价"],
              ["settlement_source", "结算价来源"],
            ] as const
          ).map(([key, label]) => (
            <label key={key}>
              {t(label)}
              <input
                aria-label={t(label)}
                required
                maxLength={256}
                inputMode={key === "settlement_price" ? "decimal" : "text"}
                value={day[key]}
                onChange={e => update(dayIndex, { [key]: e.target.value })}
              />
            </label>
          ))}
          <button
            type="button"
            disabled={days.length === 1}
            onClick={() => onChange(days.filter((_, i) => i !== dayIndex))}
          >
            {t("删除交易日")}
          </button>
        </fieldset>
      ))}
      <button
        type="button"
        disabled={days.length >= 64}
        onClick={() => onChange([...days, emptyTradingDay()])}
      >
        {t("添加交易日")}
      </button>
    </div>
  );
}
