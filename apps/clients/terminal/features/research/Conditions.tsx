import { useId, useLayoutEffect, useRef, useState, type ReactNode } from "react";
import { listSeparator, type MessageValues, translate } from "../../src/i18n";
import { warehouseName } from "../../src/ui/contract-names";
import { DeleteRecord } from "./LabParts";
import { symbolsOf, type ConditionSet } from "./research-storage";

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
  // Suggested name when saving the current draft as a preset.
  name: (fields: F) => string;
  same: (a: F, b: F) => boolean;
};

// Working condition draft decoupled from explicitly saved benchmark presets.
export function useConditions<F>(store: ConditionStore<F>, apply: (fields: F) => void) {
  const [conditions, setConditions] = useState(store.read);
  const activePreset = conditions.items.find(item => item.name === conditions.active);
  const currentFields = conditions.current ?? activePreset?.fields ?? store.defaults();
  const dirty = Boolean(activePreset && !store.same(currentFields, activePreset.fields));

  const commit = (next: ConditionSet<F>) => {
    store.write(next);
    setConditions(next);
    const preset = next.items.find(item => item.name === next.active);
    apply(next.current ?? preset?.fields ?? store.defaults());
  };

  // Update the working draft directly; never silently creates a saved preset.
  const update = (fields: F) => {
    const state = store.read();
    const matched = state.items.find(item => store.same(item.fields, fields));
    const active = state.active || (matched?.name ?? "");
    commit({ ...state, active, current: fields });
  };

  // Load settings from a past run or factor bridge into the working draft only.
  const adopt = (fields: F) => {
    const state = store.read();
    const matched = state.items.find(item => store.same(item.fields, fields));
    commit({
      ...state,
      active: matched ? matched.name : "",
      current: matched ? matched.fields : fields,
    });
    return matched?.fields ?? fields;
  };

  // Explicitly save or overwrite a named benchmark preset.
  const savePreset = (name: string, fields: F, overwriteName = "") => {
    const state = store.read();
    const trimmed = name.trim() || store.name(fields);
    const existing = state.items.find(item => item.name === trimmed);
    if (existing && trimmed === overwriteName) {
      const items = state.items.map(item =>
        item.name === trimmed ? { name: trimmed, fields } : item,
      );
      commit({ active: trimmed, current: fields, items });
      return trimmed;
    }
    const taken = state.items.map(item => item.name);
    let unique = trimmed;
    for (let number = 2; taken.includes(unique); number++) unique = `${trimmed} ${number}`;
    const items = [...state.items, { name: unique, fields }];
    commit({ active: unique, current: fields, items });
    return unique;
  };

  const choose = (name: string) => {
    const state = store.read();
    const preset = state.items.find(item => item.name === name);
    if (!preset) return;
    commit({ ...state, active: preset.name, current: preset.fields });
  };

  const revert = () => {
    const state = store.read();
    const preset = state.items.find(item => item.name === state.active);
    if (!preset) return;
    commit({ ...state, current: preset.fields });
  };

  const remove = (name: string) => {
    const state = store.read();
    const items = state.items.filter(item => item.name !== name);
    const nextActive = state.active === name ? (items[0]?.name ?? "") : state.active;
    const nextPreset = items.find(item => item.name === nextActive);
    commit({
      active: nextActive,
      current: state.active === name ? nextPreset?.fields : state.current,
      items,
    });
  };

  return {
    ...conditions,
    dirty,
    nameOf: store.name,
    update,
    adopt,
    savePreset,
    choose,
    revert,
    remove,
  };
}

// One line above a research page: optional saved benchmark preset, inline
// high-frequency controls, concise summary, and explicit save/settings triggers.
export function ConditionBar<F extends { symbols: string }>({
  title,
  setLabel,
  editLabel,
  conditions,
  text,
  initial,
  quickControls,
  fields,
}: {
  title: string;
  setLabel: string;
  editLabel: string;
  conditions: ReturnType<typeof useConditions<F>>;
  text: string;
  initial: F;
  quickControls?: ReactNode;
  fields: (form: F, change: (patch: Partial<F>) => void) => ReactNode;
}) {
  const [editing, setEditing] = useState<F | null>(null);
  const [saving, setSaving] = useState(false);
  const active = conditions.items.find(item => item.name === conditions.active);
  const hasSymbols = symbolsOf(initial.symbols).length > 0;
  const canSavePreset = hasSymbols && (!active || conditions.dirty);

  return (
    <div className="lab-scope">
      {conditions.items.length > 0 ? (
        <label className="lab-scope-preset">
          <span>{title}</span>
          <select
            aria-label={title}
            value={active ? active.name : ""}
            onChange={event => {
              if (event.target.value) conditions.choose(event.target.value);
            }}
          >
            {!active && <option value="">{t("临时条件")}</option>}
            {conditions.items.map(item => (
              <option key={item.name} value={item.name}>
                {item.name}
              </option>
            ))}
          </select>
        </label>
      ) : (
        <span>{title}</span>
      )}
      {quickControls}
      {hasSymbols && text && <p title={text}>{text}</p>}
      {conditions.dirty && (
        <>
          <span className="lab-scope-dirty">{t("已修改")}</span>
          <button type="button" className="lab-link" onClick={conditions.revert}>
            {t("恢复")}
          </button>
        </>
      )}
      {canSavePreset && (
        <button type="button" className="lab-link" onClick={() => setSaving(true)}>
          {t("存为预设")}
        </button>
      )}
      <button
        type="button"
        className={hasSymbols ? "lab-link" : "lab-scope-cta"}
        aria-label={hasSymbols ? editLabel : setLabel}
        onClick={() => setEditing(initial)}
      >
        {t(hasSymbols ? "更多设置" : "设置")}
      </button>
      {editing && (
        <ConditionDialog
          title={title}
          activeName={active?.name ?? ""}
          initial={editing}
          fields={fields}
          apply={conditions.update}
          remove={conditions.remove}
          close={() => setEditing(null)}
        />
      )}
      {saving && (
        <SavePresetDialog
          title={t("存为预设")}
          activeName={active?.name ?? ""}
          suggested={active?.name || conditions.nameOf(initial)}
          save={(name, overwrite) => conditions.savePreset(name, initial, overwrite)}
          close={() => setSaving(false)}
        />
      )}
    </div>
  );
}

function ConditionDialog<F extends { symbols: string }>({
  title,
  activeName,
  initial,
  fields,
  apply,
  remove,
  close,
}: {
  title: string;
  activeName: string;
  initial: F;
  fields: (form: F, change: (patch: Partial<F>) => void) => ReactNode;
  apply: (fields: F) => void;
  remove: (name: string) => void;
  close: () => void;
}) {
  const dialog = useRef<HTMLDialogElement>(null);
  const heading = useId();
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
      <form
        className="lab-scope-form"
        onSubmit={event => {
          event.preventDefault();
          apply(form);
          close();
        }}
      >
        {/* Focus starts here: the symbol field opens its list when focused. */}
        <h2 id={heading} tabIndex={-1}>
          {title}
        </h2>
        <div className="lab-scope-fields">
          {fields(form, patch => setForm(previous => ({ ...previous, ...patch })))}
        </div>
        <footer>
          {activeName && (
            <DeleteRecord
              name={activeName}
              remove={() => {
                remove(activeName);
                close();
              }}
            />
          )}
          <span className="lab-spacer" />
          <button type="button" onClick={close}>
            {t("取消")}
          </button>
          <button type="submit" className="primary" disabled={!hasSymbols}>
            {t("完成")}
          </button>
        </footer>
      </form>
    </dialog>
  );
}

function SavePresetDialog({
  title,
  activeName,
  suggested,
  save,
  close,
}: {
  title: string;
  activeName: string;
  suggested: string;
  save: (name: string, overwriteName?: string) => void;
  close: () => void;
}) {
  const dialog = useRef<HTMLDialogElement>(null);
  const heading = useId();
  const inputRef = useRef<HTMLInputElement>(null);
  const [label, setLabel] = useState(suggested);
  useLayoutEffect(() => {
    const element = dialog.current;
    element?.showModal();
    inputRef.current?.focus();
    inputRef.current?.select();
    return () => element?.close();
  }, []);
  return (
    <dialog
      ref={dialog}
      className="lab-module-dialog"
      aria-labelledby={heading}
      onCancel={event => {
        event.preventDefault();
        close();
      }}
    >
      <h2 id={heading}>{title}</h2>
      <form
        onSubmit={event => {
          event.preventDefault();
          const trimmed = label.trim() || suggested;
          save(trimmed, trimmed === activeName ? activeName : "");
          close();
        }}
      >
        <label>
          <span>{t("名称")}</span>
          <input
            ref={inputRef}
            value={label}
            placeholder={suggested}
            onChange={event => setLabel(event.target.value)}
          />
        </label>
        <footer>
          <button type="button" onClick={close}>
            {t("取消")}
          </button>
          <button type="submit" className="primary">
            {activeName && label.trim() === activeName ? t("更新预设") : t("保存")}
          </button>
        </footer>
      </form>
    </dialog>
  );
}
