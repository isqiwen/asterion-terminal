import type { TerminalContext } from "../../src/host/feature-registry";
import { getLocale, type MessageValues, translate } from "../../src/i18n";
import { DateInput } from "../../src/ui/DateInput";
import {
  ConditionBar,
  dayRange,
  symbolNames,
  useConditions,
  type ConditionStore,
} from "./Conditions";
import { datePlaceholders, SymbolField, type StoredCatalog, useStoredCatalog } from "./LabParts";
import {
  defaultForm,
  readConditions,
  sameScope,
  saveConditions,
  scopeOf,
  symbolsOf,
  type RunForm,
  type ScopeFields,
} from "./research-lab";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

const limits = [
  ["初始资金", "deposit"],
  ["滑点（跳）", "slippage"],
  ["单笔手数", "maxOrderLots"],
  ["单合约持仓手数", "maxContractLots"],
  ["账户总持仓手数", "maxGrossLots"],
  ["在途委托数", "maxWorkingOrders"],
  ["总权重上限", "maxGrossWeight"],
  ["净权重上限", "maxNetWeight"],
  ["保证金率上限", "maxMarginRatio"],
  ["最小调仓偏离", "minRebalanceWeight"],
  ["每年期数", "annualPeriods"],
] as const;

const periodName = (minutes: string) =>
  minutes === "0" ? t("日线") : t("{count} 分钟", { count: minutes });

const store: ConditionStore<ScopeFields> = {
  read: readConditions,
  write: saveConditions,
  defaults: () => scopeOf(defaultForm()),
  name: fields => `${symbolNames(fields.symbols)} · ${periodName(fields.minutes)}`,
};

function summary(form: RunForm, begin: string, end: string) {
  const deposit = Number(form.deposit);
  return [
    symbolNames(form.symbols),
    dayRange(form.begin, form.end, begin, end),
    periodName(form.minutes),
    Number.isFinite(deposit)
      ? new Intl.NumberFormat(getLocale(), { notation: "compact" }).format(deposit)
      : form.deposit,
    t("滑点 {count} 跳", { count: form.slippage }),
    ...(form.holdoutDay ? [t("留出 {day}", { day: form.holdoutDay })] : []),
  ].join(" · ");
}

export function useRunConditions(setForm: (update: (previous: RunForm) => RunForm) => void) {
  const conditions = useConditions(store, fields =>
    setForm(previous => ({ ...previous, ...fields })),
  );
  return {
    ...conditions,
    // A past run's settings: the set that already holds them, or a new one.
    adopt: (fields: ScopeFields) => {
      const current = readConditions();
      const same = current.items.find(item => sameScope(item.fields, fields));
      if (same) conditions.commit({ ...current, active: same.name });
      else conditions.save("", "", fields);
      return same?.fields ?? fields;
    },
  };
}

// What every backtest on this page is measured under: one of the saved sets.
export function RunScope({
  context,
  form,
  begin,
  end,
  conditions,
  catalog,
}: {
  context: TerminalContext;
  form: RunForm;
  // What blank days resolve to.
  begin: string;
  end: string;
  conditions: ReturnType<typeof useRunConditions>;
  catalog?: StoredCatalog;
}) {
  return (
    <ConditionBar
      title={t("回测条件")}
      setLabel={t("设置回测条件")}
      editLabel={t("修改回测条件")}
      conditions={conditions}
      text={conditions.items.length ? summary(form, begin, end) : ""}
      initial={scopeOf(form)}
      fields={(draft, change) => (
        <RunFields
          context={context}
          form={draft}
          change={change}
          providedCatalog={catalog}
          providedMinutes={form.minutes !== "0"}
        />
      )}
    />
  );
}

function RunFields({
  context,
  form,
  change,
  providedCatalog,
  providedMinutes,
}: {
  context: TerminalContext;
  form: ScopeFields;
  change: (patch: Partial<ScopeFields>) => void;
  providedCatalog?: StoredCatalog;
  providedMinutes?: boolean;
}) {
  const reuseProvided = providedCatalog !== undefined && providedMinutes === (form.minutes !== "0");
  const loadedCatalog = useStoredCatalog(context, form.minutes !== "0", false, !reuseProvided);
  const catalog = reuseProvided ? providedCatalog : loadedCatalog;
  const [begin, end] = datePlaceholders(catalog, symbolsOf(form.symbols), false);
  const dateField = (label: string, key: "begin" | "end" | "holdoutDay", placeholder?: string) => (
    <label>
      <span>{t(label)}</span>
      <DateInput
        value={form[key]}
        placeholder={placeholder}
        onChange={value => change({ [key]: value })}
      />
    </label>
  );
  return (
    <>
      <section>
        <h3>{t("品种与区间")}</h3>
        <div className="lab-form">
          <SymbolField
            context={context}
            value={form.symbols}
            minutes={form.minutes !== "0"}
            catalog={catalog}
            onChange={symbols => change({ symbols })}
          />
          {dateField("开始日", "begin", begin)}
          {dateField("结束日", "end", end)}
          <label>
            <span>{t("周期")}</span>
            <select
              value={form.minutes}
              onChange={event => change({ minutes: event.target.value })}
            >
              <option value="0">{t("日线")}</option>
              {[1, 5, 10, 15, 30, 60].map(value => (
                <option key={value} value={String(value)}>
                  {t("{count} 分钟", { count: value })}
                </option>
              ))}
            </select>
          </label>
          {dateField("留出日", "holdoutDay")}
        </div>
      </section>
      <section>
        <h3>{t("资金与风控")}</h3>
        <div className="lab-form">
          {limits.map(([label, key]) => (
            <label key={key}>
              <span>{t(label)}</span>
              <input value={form[key]} onChange={event => change({ [key]: event.target.value })} />
            </label>
          ))}
          <label className="lab-check wide">
            <input
              type="checkbox"
              checked={form.zeroRiskFree}
              onChange={event => change({ zeroRiskFree: event.target.checked })}
            />
            {t("无风险利率为零")}
          </label>
        </div>
      </section>
    </>
  );
}
