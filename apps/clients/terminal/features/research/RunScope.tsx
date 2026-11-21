import { useId, useLayoutEffect, useRef, useState } from "react";
import type { TerminalContext } from "../../src/host/feature-registry";
import { getLocale, listSeparator, type MessageValues, translate } from "../../src/i18n";
import { warehouseName } from "../../src/ui/contract-names";
import { DateInput } from "../../src/ui/DateInput";
import { datePlaceholders, DeleteRecord, SymbolField, useStoredCatalog } from "./LabParts";
import {
  defaultForm,
  readConditions,
  sameScope,
  saveConditions,
  scopeOf,
  symbolsOf,
  type RunConditions,
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

function symbolNames(symbols: string) {
  const names = symbolsOf(symbols).map(symbol => warehouseName(symbol) ?? symbol);
  const shown = names.slice(0, 3).join(listSeparator());
  return names.length > 3
    ? t("{names} 等 {count} 个品种", { names: shown, count: names.length })
    : shown;
}
const periodName = (minutes: string) =>
  minutes === "0" ? t("日线") : t("{count} 分钟", { count: minutes });
const conditionName = (fields: ScopeFields) =>
  `${symbolNames(fields.symbols)} · ${periodName(fields.minutes)}`;

function summary(form: RunForm, begin: string, end: string) {
  const day = (value: string, stored: string) => value || (/^\d/.test(stored) ? stored : "");
  const [first, last] = [day(form.begin, begin), day(form.end, end)];
  const deposit = Number(form.deposit);
  return [
    symbolNames(form.symbols),
    first && last
      ? `${first} – ${last}`
      : first
        ? t("{day} 起", { day: first })
        : last
          ? t("至 {day}", { day: last })
          : t("全部区间"),
    periodName(form.minutes),
    Number.isFinite(deposit)
      ? new Intl.NumberFormat(getLocale(), { notation: "compact" }).format(deposit)
      : form.deposit,
    t("滑点 {count} 跳", { count: form.slippage }),
    ...(form.holdoutDay ? [t("留出 {day}", { day: form.holdoutDay })] : []),
  ].join(" · ");
}

// The saved sets of settings, with the one in use mirrored into the form.
export function useRunConditions(setForm: (update: (previous: RunForm) => RunForm) => void) {
  const [conditions, setConditions] = useState(readConditions);
  const commit = (next: RunConditions) => {
    saveConditions(next);
    setConditions(next);
    const fields = next.items.find(item => item.name === next.active)?.fields;
    setForm(previous => ({ ...previous, ...(fields ?? scopeOf(defaultForm())) }));
  };
  // `previous` is the set being edited, or empty for a new one.
  const save = (previous: string, name: string, fields: ScopeFields) => {
    const { items } = readConditions();
    const taken = items.filter(item => item.name !== previous).map(item => item.name);
    const base = name.trim() || conditionName(fields);
    let unique = base;
    for (let number = 2; taken.includes(unique); number++) unique = `${base} ${number}`;
    const item = { name: unique, fields };
    commit({
      active: unique,
      items: previous
        ? items.map(entry => (entry.name === previous ? item : entry))
        : [...items, item],
    });
  };
  return {
    ...conditions,
    save,
    choose: (name: string) => commit({ ...readConditions(), active: name }),
    remove: (name: string) => {
      const current = readConditions();
      const items = current.items.filter(item => item.name !== name);
      commit({ active: current.active === name ? (items[0]?.name ?? "") : current.active, items });
    },
    // A past run's settings: the set that already holds them, or a new one.
    adopt: (fields: ScopeFields) => {
      const current = readConditions();
      const same = current.items.find(item => sameScope(item.fields, fields));
      if (same) commit({ ...current, active: same.name });
      else save("", "", fields);
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
}: {
  context: TerminalContext;
  form: RunForm;
  // What blank days resolve to.
  begin: string;
  end: string;
  conditions: ReturnType<typeof useRunConditions>;
}) {
  const [editing, setEditing] = useState<{ name: string; fields: ScopeFields } | null>(null);
  const active = conditions.items.find(item => item.name === conditions.active);
  const text = active ? summary(form, begin, end) : "";
  return (
    <div className="lab-scope">
      {active ? (
        <label>
          <span>{t("回测条件")}</span>
          <select
            value={active.name}
            onChange={event =>
              event.target.value
                ? conditions.choose(event.target.value)
                : setEditing({ name: "", fields: active.fields })
            }
          >
            {conditions.items.map(item => (
              <option key={item.name} value={item.name}>
                {item.name}
              </option>
            ))}
            <option value="">{t("＋ 新建")}</option>
          </select>
        </label>
      ) : (
        <span>{t("回测条件")}</span>
      )}
      {text && <p title={text}>{text}</p>}
      <button
        type="button"
        className="lab-link"
        aria-label={t(active ? "修改回测条件" : "设置回测条件")}
        onClick={() => setEditing(active ?? { name: "", fields: scopeOf(form) })}
      >
        {t(active ? "修改" : "设置")}
      </button>
      {editing && (
        <ScopeDialog
          context={context}
          name={editing.name}
          initial={editing.fields}
          save={conditions.save}
          remove={conditions.remove}
          close={() => setEditing(null)}
        />
      )}
    </div>
  );
}

function ScopeDialog({
  context,
  name,
  initial,
  save,
  remove,
  close,
}: {
  context: TerminalContext;
  // Empty for a new set.
  name: string;
  initial: ScopeFields;
  save: (previous: string, name: string, fields: ScopeFields) => void;
  remove: (name: string) => void;
  close: () => void;
}) {
  const dialog = useRef<HTMLDialogElement>(null);
  const heading = useId();
  const [title, setTitle] = useState(name);
  const [form, setForm] = useState(initial);
  const change = (patch: Partial<ScopeFields>) => setForm(previous => ({ ...previous, ...patch }));
  const symbols = symbolsOf(form.symbols);
  const catalog = useStoredCatalog(context, form.minutes !== "0");
  const [begin, end] = datePlaceholders(catalog, symbols, false);
  useLayoutEffect(() => {
    const element = dialog.current;
    element?.showModal();
    return () => element?.close();
  }, []);
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
    <dialog
      ref={dialog}
      className="lab-scope-dialog"
      aria-labelledby={heading}
      onCancel={event => {
        event.preventDefault();
        close();
      }}
    >
      {/* Focus starts here: the symbol field opens its list when focused. */}
      <h2 id={heading} tabIndex={-1}>
        {t("回测条件")}
      </h2>
      <label className="lab-scope-name">
        <span>{t("名称")}</span>
        <input
          value={title}
          placeholder={symbols.length ? conditionName(form) : undefined}
          onChange={event => setTitle(event.target.value)}
        />
      </label>
      <div className="lab-scope-fields">
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
                <input
                  value={form[key]}
                  onChange={event => change({ [key]: event.target.value })}
                />
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
      </div>
      <footer>
        {name && (
          <DeleteRecord
            name={name}
            remove={() => {
              remove(name);
              close();
            }}
          />
        )}
        <span className="lab-spacer" />
        <button type="button" onClick={close}>
          {t("取消")}
        </button>
        <button
          type="button"
          className="primary"
          disabled={!symbols.length}
          onClick={() => {
            save(name, title, form);
            close();
          }}
        >
          {t("完成")}
        </button>
      </footer>
    </dialog>
  );
}
