import { useId, useLayoutEffect, useRef, useState, type ReactNode } from "react";
import { listSeparator, type MessageValues, translate } from "../../src/i18n";
import { warehouseName } from "../../src/ui/contract-names";
import { DeleteRecord } from "./LabParts";
import { symbolsOf, type ConditionSet } from "./research-lab";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

export function symbolNames(symbols: string) {
  const names = symbolsOf(symbols).map(symbol => warehouseName(symbol) ?? symbol);
  const shown = names.slice(0, 3).join(listSeparator());
  return names.length > 3
    ? t("{names} 等 {count} 个品种", { names: shown, count: names.length })
    : shown;
}

// `storedBegin` and `storedEnd` are what blank days resolve to, once known.
export function dayRange(begin: string, end: string, storedBegin: string, storedEnd: string) {
  const day = (value: string, stored: string) => value || (/^\d/.test(stored) ? stored : "");
  const [first, last] = [day(begin, storedBegin), day(end, storedEnd)];
  if (first && last) return `${first} – ${last}`;
  if (first) return t("{day} 起", { day: first });
  return last ? t("至 {day}", { day: last }) : t("全部区间");
}

export type ConditionStore<F> = {
  read: () => ConditionSet<F>;
  write: (conditions: ConditionSet<F>) => void;
  // What the page uses with no saved set.
  defaults: () => F;
  // The name of a set left unnamed.
  name: (fields: F) => string;
};

// The saved sets of settings; `apply` mirrors the one in use into the page's form.
export function useConditions<F>(store: ConditionStore<F>, apply: (fields: F) => void) {
  const [conditions, setConditions] = useState(store.read);
  const commit = (next: ConditionSet<F>) => {
    store.write(next);
    setConditions(next);
    apply(next.items.find(item => item.name === next.active)?.fields ?? store.defaults());
  };
  // `previous` is the set being edited, or empty for a new one.
  const save = (previous: string, name: string, fields: F) => {
    const { items } = store.read();
    const taken = items.filter(item => item.name !== previous).map(item => item.name);
    const base = name.trim() || store.name(fields);
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
    nameOf: store.name,
    save,
    commit,
    choose: (name: string) => commit({ ...store.read(), active: name }),
    remove: (name: string) => {
      const current = store.read();
      const items = current.items.filter(item => item.name !== name);
      commit({ active: current.active === name ? (items[0]?.name ?? "") : current.active, items });
    },
  };
}

// One line above a research page: the set in use, what it says, and its editor.
export function ConditionBar<F extends { symbols: string }>({
  title,
  setLabel,
  editLabel,
  conditions,
  text,
  initial,
  fields,
}: {
  title: string;
  setLabel: string;
  editLabel: string;
  conditions: ReturnType<typeof useConditions<F>>;
  text: string;
  // What a first set starts from.
  initial: F;
  fields: (form: F, change: (patch: Partial<F>) => void) => ReactNode;
}) {
  const [editing, setEditing] = useState<{ name: string; fields: F } | null>(null);
  const active = conditions.items.find(item => item.name === conditions.active);
  return (
    <div className="lab-scope">
      {active ? (
        <label>
          <span>{title}</span>
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
        <span>{title}</span>
      )}
      {active ? <p title={text}>{text}</p> : <p className="subtle">{t("未设置品种与区间")}</p>}
      <button
        type="button"
        className={active ? "lab-link" : "lab-scope-cta"}
        aria-label={active ? editLabel : setLabel}
        onClick={() => setEditing(active ?? { name: "", fields: initial })}
      >
        {t(active ? "修改" : "设置")}
      </button>
      {editing && (
        <ConditionDialog
          title={title}
          name={editing.name}
          initial={editing.fields}
          nameOf={conditions.nameOf}
          fields={fields}
          save={conditions.save}
          remove={conditions.remove}
          close={() => setEditing(null)}
        />
      )}
    </div>
  );
}

function ConditionDialog<F extends { symbols: string }>({
  title,
  name,
  initial,
  nameOf,
  fields,
  save,
  remove,
  close,
}: {
  title: string;
  // Empty for a new set.
  name: string;
  initial: F;
  nameOf: (fields: F) => string;
  fields: (form: F, change: (patch: Partial<F>) => void) => ReactNode;
  save: (previous: string, name: string, fields: F) => void;
  remove: (name: string) => void;
  close: () => void;
}) {
  const dialog = useRef<HTMLDialogElement>(null);
  const heading = useId();
  const [label, setLabel] = useState(name);
  const [form, setForm] = useState(initial);
  const hasSymbols = symbolsOf(form.symbols).length > 0;
  useLayoutEffect(() => {
    const element = dialog.current;
    element?.showModal();
    return () => element?.close();
  }, []);
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
        {title}
      </h2>
      <label className="lab-scope-name">
        <span>{t("名称")}</span>
        <input
          value={label}
          placeholder={hasSymbols ? nameOf(form) : undefined}
          onChange={event => setLabel(event.target.value)}
        />
      </label>
      <div className="lab-scope-fields">
        {fields(form, patch => setForm(previous => ({ ...previous, ...patch })))}
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
          disabled={!hasSymbols}
          onClick={() => {
            save(name, label, form);
            close();
          }}
        >
          {t("完成")}
        </button>
      </footer>
    </dialog>
  );
}
