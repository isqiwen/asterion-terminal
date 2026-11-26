import type { TerminalContext } from "../../src/host/feature-registry";
import { type MessageValues, translate } from "../../src/i18n";
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
  defaultFactorForm,
  factorScopeOf,
  readFactorConditions,
  sameFactorScope,
  saveFactorConditions,
  symbolsOf,
  type FactorForm,
  type FactorScope,
} from "./research-lab";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

const store: ConditionStore<FactorScope> = {
  read: readFactorConditions,
  write: saveFactorConditions,
  defaults: () => factorScopeOf(defaultFactorForm()),
  name: fields => symbolNames(fields.symbols),
  same: sameFactorScope,
};

const FACTOR_PERIODS = [
  ["0", "日线"],
  ["60", "60m"],
  ["30", "30m"],
  ["15", "15m"],
  ["10", "10m"],
  ["5", "5m"],
  ["1", "1m"],
] as const;

function summary(form: FactorForm, begin: string, end: string) {
  return [
    dayRange(form.begin, form.end, begin, end),
    ...(form.minutes && form.minutes !== "0" ? [`${form.minutes}m`] : []),
    t("持有 {horizons} 根", { horizons: symbolsOf(form.horizons).join(", ") }),
    t("{count} 组", { count: form.groups }),
    ...(form.winsorize ? [t("MAD 去极值")] : []),
    ...(form.standardize === "zscore" ? ["Z-Score"] : []),
    ...(form.standardize === "rank" ? [t("截面排名")] : []),
    ...(form.volAdjust ? [t("波动归一")] : []),
    ...(Number(form.minVolume) > 0 ? [t("量≥{value}", { value: form.minVolume!.trim() })] : []),
    ...(Number(form.minOpenInterest) > 0
      ? [t("仓≥{value}", { value: form.minOpenInterest!.trim() })]
      : []),
    ...(Number(form.costBps) > 0 ? [t("成本 {value} bps", { value: form.costBps!.trim() })] : []),
    ...(form.holdoutDay ? [t("留出 {day}", { day: form.holdoutDay })] : []),
  ].join(" · ");
}

export function useFactorConditions(
  setForm: (update: (previous: FactorForm) => FactorForm) => void,
) {
  return useConditions(store, fields => setForm(previous => ({ ...previous, ...fields })));
}

// What every evaluation on this page is measured under: a working draft, optionally backed by a saved preset.
export function FactorScopeBar({
  context,
  form,
  begin,
  end,
  conditions,
  catalog,
}: {
  context: TerminalContext;
  form: FactorForm;
  // What blank days resolve to.
  begin: string;
  end: string;
  conditions: ReturnType<typeof useFactorConditions>;
  catalog?: StoredCatalog;
}) {
  const current = factorScopeOf(form);
  const hasSymbols = symbolsOf(form.symbols).length > 0;
  return (
    <ConditionBar
      title={t("评价条件")}
      setLabel={t("设置评价条件")}
      editLabel={t("修改评价条件")}
      conditions={conditions}
      text={hasSymbols ? summary(form, begin, end) : ""}
      initial={current}
      quickControls={
        <>
          <SymbolPopover
            context={context}
            value={form.symbols}
            allowExchanges
            catalog={catalog}
            fallbackLabel={symbolNames(form.symbols)}
            onChange={symbols => conditions.update({ ...current, symbols })}
          />
          {hasSymbols && (
            <>
              <select
                className="lab-scope-quick"
                aria-label={t("快速切换周期")}
                value={current.minutes}
                onChange={event =>
                  conditions.update({
                    ...current,
                    minutes: event.target.value,
                  })
                }
              >
                {FACTOR_PERIODS.map(([value, label]) => (
                  <option key={value} value={value}>
                    {value === "0" ? t(label) : label}
                  </option>
                ))}
              </select>
              <select
                className="lab-scope-quick"
                aria-label={t("快速切换收益口径")}
                value={current.returnMode}
                onChange={event =>
                  conditions.update({
                    ...current,
                    returnMode: event.target.value as FactorScope["returnMode"],
                  })
                }
              >
                <option value="open">{t("次根开盘")}</option>
                <option value="close">{t("收盘对收盘")}</option>
              </select>
              <select
                className="lab-scope-quick"
                aria-label={t("快速切换中性化")}
                value={current.neutralize}
                onChange={event =>
                  conditions.update({
                    ...current,
                    neutralize: event.target.value as FactorScope["neutralize"],
                  })
                }
              >
                <option value="">{t("不中性化")}</option>
                <option value="sector">{t("板块中性化")}</option>
              </select>
            </>
          )}
        </>
      }
      fields={(draft, change) => (
        <FactorFields context={context} form={draft} change={change} providedCatalog={catalog} />
      )}
    />
  );
}

function FactorFields({
  context,
  form,
  change,
  providedCatalog,
}: {
  context: TerminalContext;
  form: FactorScope;
  change: (patch: Partial<FactorScope>) => void;
  providedCatalog?: StoredCatalog;
}) {
  const isMinutes = form.minutes !== "0";
  const loadedCatalog = useStoredCatalog(context, isMinutes, true, !providedCatalog);
  const catalog = providedCatalog ?? loadedCatalog;
  const [begin, end] = datePlaceholders(catalog, symbolsOf(form.symbols), isMinutes);
  return (
    <>
      <section>
        <h3>{t("品种与区间")}</h3>
        <div className="lab-form">
          <SymbolField
            context={context}
            value={form.symbols}
            allowExchanges
            catalog={catalog}
            onChange={symbols => change({ symbols })}
          />
          <label>
            <span>{t("周期")}</span>
            <select
              value={form.minutes}
              onChange={event => change({ minutes: event.target.value })}
            >
              {FACTOR_PERIODS.map(([value, label]) => (
                <option key={value} value={value}>
                  {value === "0" ? t(label) : label}
                </option>
              ))}
            </select>
          </label>
          <label>
            <span>{t("开始日")}</span>
            <DateInput
              value={form.begin}
              placeholder={begin}
              onChange={value => change({ begin: value })}
            />
          </label>
          <label>
            <span>{t("结束日")}</span>
            <DateInput
              value={form.end}
              placeholder={end}
              onChange={value => change({ end: value })}
            />
          </label>
        </div>
      </section>
      <section>
        <h3>{t("评价设置")}</h3>
        <div className="lab-form">
          <label>
            <span>{t("持有期（K 线数）")}</span>
            <input
              value={form.horizons}
              onChange={event => change({ horizons: event.target.value })}
            />
          </label>
          <label>
            <span>{t("分组数")}</span>
            <input value={form.groups} onChange={event => change({ groups: event.target.value })} />
          </label>
          <label>
            <span>{t("收益口径")}</span>
            <select
              value={form.returnMode}
              onChange={event =>
                change({ returnMode: event.target.value as FactorScope["returnMode"] })
              }
            >
              <option value="open">{t("次根开盘（可执行）")}</option>
              <option value="close">{t("收盘对收盘")}</option>
            </select>
          </label>
          <label>
            <span>{t("留出日")}</span>
            <DateInput value={form.holdoutDay} onChange={holdoutDay => change({ holdoutDay })} />
          </label>
        </div>
      </section>
      <section>
        <h3>{t("清洗与过滤")}</h3>
        <div className="lab-form">
          <label>
            <span>{t("截面处理")}</span>
            <select
              value={form.standardize}
              onChange={event =>
                change({ standardize: event.target.value as FactorScope["standardize"] })
              }
            >
              <option value="">{t("原始值")}</option>
              <option value="zscore">Z-Score</option>
              <option value="rank">{t("截面排名")}</option>
            </select>
          </label>
          <label>
            <span>{t("中性化")}</span>
            <select
              value={form.neutralize}
              onChange={event =>
                change({ neutralize: event.target.value as FactorScope["neutralize"] })
              }
            >
              <option value="">{t("不中性化")}</option>
              <option value="sector">{t("板块中性化")}</option>
            </select>
          </label>
          <label>
            <span>{t("最低日均成交量")}</span>
            <input
              placeholder={t("不限")}
              value={form.minVolume}
              onChange={event => change({ minVolume: event.target.value })}
            />
          </label>
          <label>
            <span>{t("最低日均持仓量")}</span>
            <input
              placeholder={t("不限")}
              value={form.minOpenInterest}
              onChange={event => change({ minOpenInterest: event.target.value })}
            />
          </label>
          <label className="wide">
            <span>{t("单边成本（bps）")}</span>
            <input
              placeholder="0"
              value={form.costBps}
              onChange={event => change({ costBps: event.target.value })}
            />
          </label>
          <label className="lab-check">
            <input
              type="checkbox"
              checked={form.winsorize}
              onChange={event => change({ winsorize: event.target.checked })}
            />
            {t("MAD 去极值")}
          </label>
          <label className="lab-check">
            <input
              type="checkbox"
              checked={form.volAdjust}
              onChange={event => change({ volAdjust: event.target.checked })}
            />
            {t("波动率归一化收益")}
          </label>
        </div>
      </section>
    </>
  );
}
