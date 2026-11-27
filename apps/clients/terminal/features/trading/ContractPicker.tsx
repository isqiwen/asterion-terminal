import { useId, useMemo, useState } from "react";
import { t } from "./live-shared";

export type ContractOption = {
  id: string;
  label?: string;
  isProduct?: boolean;
};

// Searchable combobox supporting both contract codes (SHFE.rb2610) and localized
// product names (螺纹钢 / Rebar), while keeping direct code entry + "添加" working.
export function ContractPicker({
  ariaLabel,
  label,
  options,
  disabled,
  maxReached,
  onAdd,
}: {
  ariaLabel: string;
  label?: string;
  options: readonly ContractOption[];
  disabled?: boolean;
  maxReached?: boolean;
  onAdd: (id: string) => void;
}) {
  const [query, setQuery] = useState("");
  const [open, setOpen] = useState(false);
  const [activeIndex, setActiveIndex] = useState(0);
  const listId = useId();

  const trimmed = query.trim();
  const matched = useMemo(() => {
    if (!trimmed) return undefined;
    const lower = trimmed.toLowerCase();
    return (
      options.find(item => item.id === trimmed) ??
      options.find(item => item.id.toLowerCase() === lower)
    );
  }, [options, trimmed]);

  const filtered = useMemo(() => {
    const lower = trimmed.toLowerCase();
    const list = lower
      ? options.filter(
          item =>
            item.id.toLowerCase().includes(lower) ||
            (item.label ?? "").toLowerCase().includes(lower),
        )
      : options;
    return list.slice(0, 12);
  }, [options, trimmed]);

  const commit = (id: string) => {
    if (disabled || maxReached) return;
    onAdd(id);
    setQuery("");
    setActiveIndex(0);
    setOpen(false);
  };

  const input = (
    <input
      role="combobox"
      aria-label={ariaLabel}
      aria-expanded={open && filtered.length > 0}
      aria-controls={listId}
      aria-autocomplete="list"
      spellCheck={false}
      autoCorrect="off"
      autoCapitalize="none"
      placeholder={t("输入代码或中文品种名，如 SHFE.rb / 螺纹钢")}
      value={query}
      disabled={disabled}
      onFocus={() => setOpen(true)}
      onBlur={() => setOpen(false)}
      onChange={event => {
        setQuery(event.target.value);
        setActiveIndex(0);
        setOpen(true);
      }}
      onKeyDown={event => {
        if (event.nativeEvent.isComposing) return;
        if (event.key === "ArrowDown") {
          event.preventDefault();
          setOpen(true);
          setActiveIndex(current => Math.min(filtered.length - 1, current + 1));
        } else if (event.key === "ArrowUp") {
          event.preventDefault();
          setActiveIndex(current => Math.max(0, current - 1));
        } else if (event.key === "Escape") {
          setOpen(false);
        } else if (event.key === "Enter" && open && filtered[activeIndex]) {
          event.preventDefault();
          commit(filtered[activeIndex]!.id);
        }
      }}
    />
  );

  return (
    <div className="contract-picker">
      <div className="contract-picker-input">
        {label ? (
          <label>
            {label}
            {input}
          </label>
        ) : (
          input
        )}
        {open && !disabled && (trimmed || filtered.length > 0) && (
          <ul id={listId} role="listbox" className="contract-picker-menu">
            {filtered.length > 0 ? (
              filtered.map((item, index) => (
                <li
                  key={item.id}
                  role="option"
                  aria-selected={index === activeIndex}
                  onMouseDown={event => {
                    event.preventDefault();
                    commit(item.id);
                  }}
                  onMouseEnter={() => setActiveIndex(index)}
                >
                  <span className="contract-picker-id">{item.id}</span>
                  {item.label && <small>{item.label}</small>}
                </li>
              ))
            ) : (
              <li className="contract-picker-empty">{t("未找到匹配品种或合约")}</li>
            )}
          </ul>
        )}
      </div>
      <button
        type="button"
        disabled={disabled || maxReached || !matched}
        onClick={() => matched && commit(matched.id)}
      >
        {t("添加")}
      </button>
    </div>
  );
}
