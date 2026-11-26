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
import {
  datePlaceholders,
  SymbolField,
  SymbolPopover,
  type StoredCatalog,
  useStoredCatalog,
} from "./LabParts";
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

const costFields = [
  ["初始资金", "deposit", ""],
  ["滑点（跳）", "slippage", ""],
  ["佣金倍率", "feeMultiplier", "1"],
] as const;

const riskLimitFields = [
  ["单笔手数", "maxOrderLots", ""],
  ["单合约持仓手数", "maxContractLots", ""],
  ["账户总持仓手数", "maxGrossLots", ""],
  ["在途委托数", "maxWorkingOrders", ""],
] as const;

const portfolioFields = [
  ["总权重上限", "maxGrossWeight", "如 1.0"],
  ["净权重上限", "maxNetWeight", "如 1.0"],
  ["保证金率上限", "maxMarginRatio", "0–1，如 0.8"],
  ["最小调仓偏离", "minRebalanceWeight", "如 0.05"],
  ["每年期数", "annualPeriods", "252"],
] as const;

const periodName = (minutes: string) =>
  minutes === "0" ? t("日线") : t("{count} 分钟", { count: minutes });

const store: ConditionStore<ScopeFields> = {
  read: readConditions,
  write: saveConditions,
  defaults: () => scopeOf(defaultForm()),
  name: fields => `${symbolNames(fields.symbols)} · ${periodName(fields.minutes)}`,
  same: sameScope,
};

function summary(form: RunForm, begin: string, end: string) {
  const deposit = Number(form.deposit);
  const feeMult = form.feeMultiplier.trim();
  return [
    dayRange(form.begin, form.end, begin, end),
    Number.isFinite(deposit)
      ? new Intl.NumberFormat(getLocale(), { notation: "compact" }).format(deposit)
      : form.deposit,
    t("滑点 {count} 跳", { count: form.slippage }),
    ...(feeMult && Number(feeMult) !== 1 ? [t("佣金 ×{mult}", { mult: feeMult })] : []),
    ...(form.holdoutDay ? [t("留出 {day}", { day: form.holdoutDay })] : []),
  ].join(" · ");
}

export function useRunConditions(setForm: (update: (previous: RunForm) => RunForm) => void) {
  return useConditions(store, fields => setForm(previous => ({ ...previous, ...fields })));
}

// What every backtest on this page is measured under: a working draft, optionally backed by a saved preset.
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
  const current = scopeOf(form);
  const hasSymbols = symbolsOf(form.symbols).length > 0;
  return (
    <ConditionBar
      title={t("回测条件")}
      setLabel={t("设置回测条件")}
      editLabel={t("修改回测条件")}
      conditions={conditions}
      text={hasSymbols ? summary(form, begin, end) : ""}
      initial={current}
      quickControls={
        <>
          <SymbolPopover
            context={context}
            value={form.symbols}
            minutes={form.minutes !== "0"}
            catalog={catalog}
            fallbackLabel={symbolNames(form.symbols)}
            onChange={symbols => conditions.update({ ...current, symbols })}
          />
          {hasSymbols && (
            <select
              className="lab-scope-quick"
              aria-label={t("快速切换周期")}
              value={form.minutes}
              onChange={event => conditions.update({ ...current, minutes: event.target.value })}
            >
              <option value="0">{t("日线")}</option>
              {[1, 5, 10, 15, 30, 60].map(value => (
                <option key={value} value={String(value)}>
                  {t("{count} 分钟", { count: value })}
                </option>
              ))}
            </select>
          )}
        </>
      }
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
  const renderField = ([label, key, placeholder]: readonly [
    string,
    Exclude<keyof ScopeFields, "zeroRiskFree">,
    string,
  ]) => (
    <label key={key}>
      <span>{t(label)}</span>
      <input
        className="lab-param"
        placeholder={
          placeholder
            ? /^\d+(\.\d+)?$/.test(placeholder)
              ? placeholder
              : t(placeholder)
            : undefined
        }
        value={form[key]}
        onChange={event => change({ [key]: event.target.value })}
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
      <div className="lab-scope-groups">
        <section className="lab-scope-group">
          <h3>{t("资金与成本")}</h3>
          <div className="lab-form">{costFields.map(renderField)}</div>
        </section>
        <section className="lab-scope-group">
          <h3>{t("风控限额")}</h3>
          <div className="lab-form">{riskLimitFields.map(renderField)}</div>
        </section>
        <section className="lab-scope-group">
          <h3>{t("组合与统计")}</h3>
          <div className="lab-form">
            {portfolioFields.map(renderField)}
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
      </div>
    </>
  );
}
