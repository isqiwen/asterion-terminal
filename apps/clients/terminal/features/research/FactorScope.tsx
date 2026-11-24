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
import { datePlaceholders, SymbolField, type StoredCatalog, useStoredCatalog } from "./LabParts";
import {
  defaultFactorForm,
  factorScopeOf,
  readFactorConditions,
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
};

function summary(form: FactorForm, begin: string, end: string) {
  return [
    symbolNames(form.symbols),
    dayRange(form.begin, form.end, begin, end),
    t("持有 {horizons} 根", { horizons: symbolsOf(form.horizons).join(", ") }),
    t("{count} 组", { count: form.groups }),
    ...(form.standardize === "zscore" ? ["Z-Score"] : []),
    ...(form.standardize === "rank" ? [t("截面排名")] : []),
    ...(form.holdoutDay ? [t("留出 {day}", { day: form.holdoutDay })] : []),
  ].join(" · ");
}

export function useFactorConditions(
  setForm: (update: (previous: FactorForm) => FactorForm) => void,
) {
  return useConditions(store, fields => setForm(previous => ({ ...previous, ...fields })));
}

// What every evaluation on this page is measured under: one of the saved sets.
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
  return (
    <ConditionBar
      title={t("评价条件")}
      setLabel={t("设置评价条件")}
      editLabel={t("修改评价条件")}
      conditions={conditions}
      text={conditions.items.length ? summary(form, begin, end) : ""}
      initial={factorScopeOf(form)}
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
  const loadedCatalog = useStoredCatalog(context, false, true, !providedCatalog);
  const catalog = providedCatalog ?? loadedCatalog;
  const [begin, end] = datePlaceholders(catalog, symbolsOf(form.symbols), false);
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
            <span>{t("留出日")}</span>
            <DateInput value={form.holdoutDay} onChange={holdoutDay => change({ holdoutDay })} />
          </label>
        </div>
      </section>
    </>
  );
}
