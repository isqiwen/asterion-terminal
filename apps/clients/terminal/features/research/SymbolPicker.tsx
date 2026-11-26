import { useCallback, useEffect, useId, useMemo, useRef, useState } from "react";
import { lab } from "../../src/bridge/lab";
import type { StoredContract } from "../../src/generated/asterion/lab_pb";
import type { TerminalContext } from "../../src/host/feature-registry";
import { getLocale, listSeparator, type MessageValues, translate } from "../../src/i18n";
import { warehouseName } from "../../src/ui/contract-names";
import {
  builtInUniverses,
  readUniverses,
  sameSymbolList,
  saveUniverses,
  subscribeUniverses,
  symbolsOf,
  type Universe,
} from "./research-storage";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

const EXCHANGES = ["SHFE", "INE", "DCE", "CZCE", "CFFEX", "GFEX"];

type SymbolSpan = { id: string; name?: string; first: string; last: string; stored: boolean };

function mergeSpan(
  map: Map<string, SymbolSpan>,
  id: string,
  first: string,
  last: string,
  name?: string,
) {
  const existing = map.get(id);
  if (!existing) {
    map.set(id, { id, name, first, last, stored: true });
    return;
  }
  existing.stored = true;
  if (name && !existing.name) existing.name = name;
  if (first && (!existing.first || first < existing.first)) existing.first = first;
  if (last && last > existing.last) existing.last = last;
}

function normalizeSymbol(raw: string, allowExchanges: boolean) {
  const trimmed = raw.trim();
  const upper = trimmed.toUpperCase();
  if (allowExchanges && EXCHANGES.includes(upper)) return upper;
  const match = /^([A-Za-z]+)\/([A-Za-z]{1,4})(?:\/(\d{4}-\d{2}))?$/.exec(trimmed);
  if (!match) return null;
  const venue = match[1]!.toUpperCase();
  if (!EXCHANGES.includes(venue)) return null;
  const product = match[2]!.toLowerCase();
  return match[3] ? `${venue}/${product}/${match[3]}` : `${venue}/${product}`;
}

export function useStoredCatalog(
  context: TerminalContext,
  minutes = false,
  allowExchanges = false,
  enabled = true,
) {
  const endpoint = context.snapshot?.warehouse_endpoint;
  const address =
    enabled && endpoint && "host" in endpoint
      ? `${endpoint.host}:${endpoint.port}/${endpoint.fingerprint}`
      : "";
  const [contracts, setContracts] = useState<StoredContract[] | null>(() => (address ? null : []));

  useEffect(() => {
    let active = true;
    if (!address) {
      setContracts([]);
      return;
    }
    setContracts(null);
    void lab({ case: "listStoredContracts", value: {} })
      .then(reply => active && setContracts(reply.contracts))
      .catch(() => active && setContracts([]));
    return () => {
      active = false;
    };
  }, [address]);

  const locale = getLocale();
  return useMemo(() => {
    const byExchange = new Map<string, SymbolSpan>();
    const byProduct = new Map<string, SymbolSpan>();
    const byContract = new Map<string, SymbolSpan>();
    for (const item of contracts ?? []) {
      const range = minutes ? item.minutes : item.daily;
      if (!range?.days) continue;
      const [exchange, code] = item.contract.split("/");
      if (!exchange || !code) continue;
      const product = `${exchange}/${code}`;
      mergeSpan(byExchange, exchange, range.firstDay, range.lastDay);
      mergeSpan(byProduct, product, range.firstDay, range.lastDay, warehouseName(product));
      mergeSpan(
        byContract,
        item.contract,
        range.firstDay,
        range.lastDay,
        warehouseName(item.contract, item.name),
      );
    }
    for (const exchange of EXCHANGES) {
      if (!byExchange.has(exchange)) {
        byExchange.set(exchange, { id: exchange, first: "", last: "", stored: false });
      }
    }
    const exchangeRank = (id: string) => {
      const index = EXCHANGES.indexOf(id.split("/")[0]!);
      return index === -1 ? EXCHANGES.length : index;
    };
    const compareId = (a: string, b: string) => (a < b ? -1 : a > b ? 1 : 0);
    const sortSpans = (a: SymbolSpan, b: SymbolSpan) =>
      Number(b.stored) - Number(a.stored) ||
      exchangeRank(a.id) - exchangeRank(b.id) ||
      compareId(a.id, b.id);
    const contractsByProduct = new Map<string, SymbolSpan[]>();
    const sortedContracts = [...byContract.values()].sort((a, b) => compareId(a.id, b.id));
    for (const item of sortedContracts) {
      const product = item.id.split("/").slice(0, 2).join("/");
      const list = contractsByProduct.get(product);
      if (list) list.push(item);
      else contractsByProduct.set(product, [item]);
    }
    return {
      ready: contracts !== null,
      exchanges: [...byExchange.values()].sort(sortSpans),
      products: [...byProduct.values()].sort(sortSpans),
      contracts: sortedContracts,
      contractsByProduct,
      lookup: (symbol: string) =>
        byContract.get(symbol) ??
        byProduct.get(symbol) ??
        (allowExchanges ? byExchange.get(symbol) : undefined),
    };
    // warehouseName depends on the current locale.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [contracts, minutes, allowExchanges, locale]);
}

export type StoredCatalog = ReturnType<typeof useStoredCatalog>;

export function datePlaceholders(
  catalog: StoredCatalog,
  symbols: readonly string[],
  separate: boolean,
): [string, string] {
  const matched = catalog.ready
    ? symbols
        .map(symbol => catalog.lookup(symbol))
        .filter((item): item is SymbolSpan => Boolean(item?.stored))
    : [];
  if (!matched.length) return ["YYYY-MM-DD", "YYYY-MM-DD"];
  if (separate && symbols.length > 1) {
    const firsts = new Set(matched.map(item => item.first));
    const lasts = new Set(matched.map(item => item.last));
    return [
      firsts.size === 1 ? matched[0]!.first : t("各自最早"),
      lasts.size === 1 ? matched[0]!.last : t("各自最新"),
    ];
  }
  const overlapFirst = matched.reduce((max, item) => (item.first > max ? item.first : max), "");
  const overlapLast = matched.reduce(
    (min, item) => (!min || (item.last && item.last < min) ? item.last : min),
    "",
  );
  if (overlapFirst && overlapLast && overlapFirst <= overlapLast)
    return [overlapFirst, overlapLast];
  const unionFirst = matched.reduce(
    (min, item) => (!min || (item.first && item.first < min) ? item.first : min),
    "",
  );
  const unionLast = matched.reduce((max, item) => (item.last > max ? item.last : max), "");
  return [unionFirst || "YYYY-MM-DD", unionLast || "YYYY-MM-DD"];
}

export function useUniverses(catalog?: StoredCatalog) {
  const [custom, setCustom] = useState<Universe[]>(readUniverses);
  useEffect(() => subscribeUniverses(() => setCustom(readUniverses())), []);
  const storedProducts = useMemo(
    () => (catalog?.products ?? []).filter(item => item.stored).map(item => item.id),
    [catalog],
  );
  const builtIn = useMemo(() => builtInUniverses(storedProducts), [storedProducts]);
  const save = useCallback((name: string, symbols: string) => {
    const trimmed = name.trim();
    if (!trimmed || !symbolsOf(symbols).length) return;
    const current = readUniverses();
    const next = current.some(item => item.name === trimmed)
      ? current.map(item => (item.name === trimmed ? { name: trimmed, symbols } : item))
      : [...current, { name: trimmed, symbols }];
    saveUniverses(next);
  }, []);
  const remove = useCallback((name: string) => {
    saveUniverses(readUniverses().filter(item => item.name !== name));
  }, []);
  return { custom, builtIn, save, remove };
}

export function universeLabel(
  symbols: string,
  universes: { custom: readonly Universe[]; builtIn: readonly Universe[] },
) {
  if (!symbolsOf(symbols).length) return "";
  const match =
    universes.custom.find(item => sameSymbolList(item.symbols, symbols)) ??
    universes.builtIn.find(item => sameSymbolList(item.symbols, symbols));
  if (!match) return "";
  return `${match.name} (${symbolsOf(symbols).length})`;
}

export function SymbolField({
  context,
  value,
  minutes = false,
  allowExchanges = false,
  catalog: providedCatalog,
  inlineMenu = false,
  onChange,
}: {
  context: TerminalContext;
  value: string;
  minutes?: boolean;
  allowExchanges?: boolean;
  catalog?: StoredCatalog;
  inlineMenu?: boolean;
  onChange: (symbols: string) => void;
}) {
  const inputId = useId();
  const listboxId = useId();
  const fieldRef = useRef<HTMLDivElement>(null);
  const inputRef = useRef<HTMLInputElement>(null);
  const internalCatalog = useStoredCatalog(context, minutes, allowExchanges, !providedCatalog);
  const catalog = providedCatalog ?? internalCatalog;
  const universes = useUniverses(catalog);
  const [open, setOpen] = useState(false);
  const menuOpen = inlineMenu || open;
  const [savingPool, setSavingPool] = useState(false);
  const [poolName, setPoolName] = useState("");
  const [query, setQuery] = useState("");
  const [venue, setVenue] = useState("");
  const [expanded, setExpanded] = useState("");
  const [activeIndex, setActiveIndex] = useState(0);

  useEffect(() => {
    if (!open || inlineMenu) return;
    const dismiss = (event: MouseEvent) => {
      if (!fieldRef.current?.contains(event.target as Node)) setOpen(false);
    };
    document.addEventListener("mousedown", dismiss);
    return () => document.removeEventListener("mousedown", dismiss);
  }, [open, inlineMenu]);

  const entered = symbolsOf(value);
  const selected = new Set(entered);
  const missing = catalog.ready ? entered.filter(symbol => !catalog.lookup(symbol)?.stored) : [];

  const needle = query.trim().toUpperCase();
  const matches = (item: SymbolSpan) => {
    if (!needle) return true;
    const parts = item.id.split("/");
    const compact =
      parts.length === 3 ? `${parts[1]}${parts[2]!.slice(2, 4)}${parts[2]!.slice(5, 7)}` : "";
    return `${item.id} ${item.name ?? ""} ${compact}`.toUpperCase().includes(needle);
  };
  const visibleExchanges = allowExchanges
    ? catalog.exchanges.filter(item => (needle ? matches(item) : !venue || item.id === venue))
    : [];
  const visibleProducts = catalog.products.filter(item =>
    needle ? matches(item) : !venue || item.id.startsWith(`${venue}/`),
  );
  const visibleContracts = needle ? catalog.contracts.filter(matches).slice(0, 24) : [];
  const typedId = needle ? normalizeSymbol(query, allowExchanges) : null;
  const typedExtra =
    typedId && !catalog.lookup(typedId)
      ? {
          id: typedId,
          name: warehouseName(typedId),
          first: "",
          last: "",
          stored: false,
        }
      : null;
  const visibleOptions: SymbolSpan[] = [
    ...(typedExtra ? [typedExtra] : []),
    ...visibleExchanges,
    ...visibleProducts.flatMap(item => [
      item,
      ...(!needle && expanded === item.id ? (catalog.contractsByProduct.get(item.id) ?? []) : []),
    ]),
    ...visibleContracts,
  ];
  const clampedActive =
    visibleOptions.length > 0 ? Math.min(activeIndex, visibleOptions.length - 1) : -1;
  const activeOption = clampedActive >= 0 ? visibleOptions[clampedActive]! : null;
  const optionId = (id: string) => `${listboxId}-${id.replace(/[^A-Za-z0-9_-]/g, "_")}`;
  useEffect(() => {
    setActiveIndex(0);
  }, [query, venue, open]);

  const toggle = (item: SymbolSpan) => {
    const adding = !selected.has(item.id);
    const next = adding ? [...entered, item.id] : entered.filter(sym => sym !== item.id);
    onChange(next.join(", "));
    if (query) setQuery("");
  };
  const commitInput = (raw: string) => {
    const parts = symbolsOf(raw);
    if (!parts.length) return false;
    const normalized = parts.map(part => normalizeSymbol(part, allowExchanges));
    if (normalized.some(item => !item)) return false;
    const next = [...entered];
    for (const sym of normalized as string[]) {
      if (!next.includes(sym)) next.push(sym);
    }
    onChange(next.join(", "));
    setQuery("");
    return true;
  };
  const openDataset = () =>
    context.navigate("workspace.data-dataset", {
      params: { kind: minutes ? "minutes" : "daily" },
    });

  const renderOption = (item: SymbolSpan, extraClass?: string, badge?: string) => {
    const active = selected.has(item.id);
    const highlighted = activeOption?.id === item.id;
    return (
      <button
        key={item.id}
        id={optionId(item.id)}
        type="button"
        role="option"
        aria-selected={active}
        data-active={highlighted || undefined}
        className={`lab-symbol-option${extraClass ? ` ${extraClass}` : ""}`}
        onMouseEnter={() => {
          const idx = visibleOptions.findIndex(opt => opt.id === item.id);
          if (idx >= 0) setActiveIndex(idx);
        }}
        onMouseDown={event => event.preventDefault()}
        onClick={() => toggle(item)}
      >
        <span className="lab-symbol-check" aria-hidden="true">
          {active ? "✓" : ""}
        </span>
        <span className="lab-symbol-main">
          {item.name && <span>{item.name}</span>}
          <code>{item.id}</code>
          {badge && <small>{badge}</small>}
        </span>
        <small className={item.stored ? "stored" : "unstored"}>
          {item.stored ? `${item.first.slice(0, 7)}–${item.last.slice(0, 7)}` : t("未入库")}
        </small>
      </button>
    );
  };

  return (
    <>
      <div
        ref={fieldRef}
        className="wide lab-symbol-field"
        data-open={menuOpen || undefined}
        data-inline={inlineMenu || undefined}
        onBlur={event => {
          if (fieldRef.current?.contains(event.relatedTarget as Node)) return;
          if (query.trim()) commitInput(query);
          setQuery("");
          if (!inlineMenu) setOpen(false);
        }}
      >
        <div className="lab-symbol-header">
          <label htmlFor={inputId}>{t("品种")}</label>
          <span className="lab-symbol-actions">
            {entered.length > 0 && !savingPool && (
              <button
                type="button"
                className="lab-link"
                onClick={() => {
                  setPoolName("");
                  setSavingPool(true);
                }}
              >
                {t("存为品种池")}
              </button>
            )}
            {entered.length > 1 && (
              <button type="button" className="lab-link" onClick={() => onChange("")}>
                {t("清空")}
              </button>
            )}
          </span>
        </div>
        {savingPool && (
          <div className="lab-pool-save">
            <input
              aria-label={t("品种池名称")}
              placeholder={t("如 黑色主力")}
              value={poolName}
              onChange={event => setPoolName(event.target.value)}
              onKeyDown={event => {
                if (event.key === "Enter" && !event.nativeEvent.isComposing) {
                  event.preventDefault();
                  if (poolName.trim()) {
                    universes.save(poolName, entered.join(", "));
                    setSavingPool(false);
                  }
                } else if (event.key === "Escape") {
                  event.preventDefault();
                  setSavingPool(false);
                }
              }}
            />
            <button
              type="button"
              className="primary"
              disabled={!poolName.trim()}
              onClick={() => {
                universes.save(poolName, entered.join(", "));
                setSavingPool(false);
              }}
            >
              {t("保存")}
            </button>
            <button type="button" className="lab-link" onClick={() => setSavingPool(false)}>
              {t("取消")}
            </button>
          </div>
        )}
        <div
          className="lab-symbol-box"
          onClick={() => {
            inputRef.current?.focus();
            setOpen(true);
          }}
        >
          {entered.map(symbol => {
            const label = warehouseName(symbol);
            return (
              <span key={symbol} className="lab-symbol-tag">
                {label && <span>{label}</span>}
                <code>{symbol}</code>
                <button
                  type="button"
                  aria-label={t("移除 {symbol}", { symbol })}
                  onClick={event => {
                    event.stopPropagation();
                    onChange(entered.filter(item => item !== symbol).join(", "));
                  }}
                >
                  ×
                </button>
              </span>
            );
          })}
          <input
            ref={inputRef}
            id={inputId}
            role="combobox"
            aria-expanded={menuOpen}
            aria-controls={listboxId}
            aria-activedescendant={menuOpen && activeOption ? optionId(activeOption.id) : undefined}
            aria-autocomplete="list"
            value={query}
            placeholder={
              entered.length ? "" : t(allowExchanges ? "选择品种或交易所…" : "选择品种或合约…")
            }
            spellCheck={false}
            autoCorrect="off"
            autoCapitalize="none"
            autoComplete="off"
            onFocus={() => setOpen(true)}
            onChange={event => {
              const raw = event.target.value;
              if (/[,，]/.test(raw) && commitInput(raw)) return;
              setQuery(raw);
              setOpen(true);
            }}
            onKeyDown={event => {
              if (event.key === "Backspace" && !query && entered.length > 0) {
                onChange(entered.slice(0, -1).join(", "));
                return;
              }
              if (event.key === "Escape" && open && !inlineMenu) {
                event.preventDefault();
                event.stopPropagation();
                setOpen(false);
                return;
              }
              if (event.key === "ArrowDown") {
                event.preventDefault();
                if (!menuOpen) {
                  setOpen(true);
                  return;
                }
                if (visibleOptions.length > 0) {
                  const next = (clampedActive + 1) % visibleOptions.length;
                  setActiveIndex(next);
                  document
                    .getElementById(optionId(visibleOptions[next]!.id))
                    ?.scrollIntoView({ block: "nearest" });
                }
                return;
              }
              if (event.key === "ArrowUp" && menuOpen && visibleOptions.length > 0) {
                event.preventDefault();
                const next = (clampedActive - 1 + visibleOptions.length) % visibleOptions.length;
                setActiveIndex(next);
                document
                  .getElementById(optionId(visibleOptions[next]!.id))
                  ?.scrollIntoView({ block: "nearest" });
                return;
              }
              if (event.key === "Enter" && !event.nativeEvent.isComposing) {
                event.preventDefault();
                if (commitInput(query)) return;
                if (activeOption) toggle(activeOption);
              }
            }}
          />
        </div>
        {menuOpen && (
          <div className="lab-symbol-menu">
            {!needle && (universes.custom.length > 0 || universes.builtIn.length > 0) && (
              <div className="lab-symbol-pools" role="group" aria-label={t("品种池")}>
                {universes.custom.map(pool => (
                  <span key={`custom:${pool.name}`} className="lab-pool-chip is-custom">
                    <button
                      type="button"
                      aria-pressed={sameSymbolList(value, pool.symbols)}
                      onMouseDown={event => event.preventDefault()}
                      onClick={() => onChange(pool.symbols)}
                    >
                      {pool.name} ({symbolsOf(pool.symbols).length})
                    </button>
                    <button
                      type="button"
                      className="lab-pool-del"
                      aria-label={t("删除品种池 {name}", { name: pool.name })}
                      onMouseDown={event => event.preventDefault()}
                      onClick={() => universes.remove(pool.name)}
                    >
                      ×
                    </button>
                  </span>
                ))}
                {universes.builtIn.map(pool => (
                  <button
                    key={`sector:${pool.name}`}
                    type="button"
                    className="lab-pool-chip"
                    aria-pressed={sameSymbolList(value, pool.symbols)}
                    onMouseDown={event => event.preventDefault()}
                    onClick={() => onChange(pool.symbols)}
                  >
                    {pool.name} ({symbolsOf(pool.symbols).length})
                  </button>
                ))}
              </div>
            )}
            {!needle && (
              <div className="lab-symbol-venues" role="group" aria-label={t("交易所")}>
                <button
                  type="button"
                  aria-pressed={!venue}
                  onMouseDown={event => event.preventDefault()}
                  onClick={() => setVenue("")}
                >
                  {t("全部")}
                </button>
                {EXCHANGES.map(item => (
                  <button
                    key={item}
                    type="button"
                    aria-pressed={venue === item}
                    onMouseDown={event => event.preventDefault()}
                    onClick={() => setVenue(venue === item ? "" : item)}
                  >
                    {item}
                  </button>
                ))}
              </div>
            )}
            <div
              id={listboxId}
              className="lab-symbol-options"
              role="listbox"
              aria-multiselectable="true"
              aria-label={t("品种")}
            >
              {typedExtra && renderOption(typedExtra)}
              {visibleExchanges.map(item => renderOption(item, undefined, t("整个交易所")))}
              {visibleProducts.map(item => {
                const months = catalog.contractsByProduct.get(item.id);
                const isExpanded = !needle && expanded === item.id;
                return (
                  <div key={item.id} className="lab-symbol-group">
                    <div className="lab-symbol-row">
                      {renderOption(item)}
                      {!needle && months && months.length > 0 && (
                        <button
                          type="button"
                          className="lab-symbol-months"
                          aria-expanded={isExpanded}
                          aria-label={t("{product} 月份合约", { product: item.id })}
                          onMouseDown={event => event.preventDefault()}
                          onClick={() => setExpanded(isExpanded ? "" : item.id)}
                        >
                          {months.length} {isExpanded ? "▾" : "▸"}
                        </button>
                      )}
                    </div>
                    {isExpanded && months?.map(month => renderOption(month, "is-contract"))}
                  </div>
                );
              })}
              {visibleContracts.map(item => renderOption(item, "is-contract"))}
              {!typedExtra &&
                !visibleExchanges.length &&
                !visibleProducts.length &&
                !visibleContracts.length && (
                  <p className="subtle lab-symbol-empty">{t("没有匹配的品种")}</p>
                )}
            </div>
          </div>
        )}
      </div>
      {missing.length > 0 && (
        <div className="wide lab-symbol-assist">
          <span className="warn">
            {t("未入库：{symbols}", { symbols: missing.join(listSeparator()) })}
          </span>
          <button type="button" className="lab-link" onClick={openDataset}>
            {t("去采集")}
          </button>
        </div>
      )}
    </>
  );
}

// Compact symbol-pool picker for the top condition bar.
export function SymbolPopover({
  context,
  value,
  minutes = false,
  allowExchanges = false,
  catalog: providedCatalog,
  fallbackLabel,
  onChange,
}: {
  context: TerminalContext;
  value: string;
  minutes?: boolean;
  allowExchanges?: boolean;
  catalog?: StoredCatalog;
  fallbackLabel: string;
  onChange: (symbols: string) => void;
}) {
  const wrapRef = useRef<HTMLDivElement>(null);
  const [open, setOpen] = useState(false);
  const internalCatalog = useStoredCatalog(context, minutes, allowExchanges, !providedCatalog);
  const catalog = providedCatalog ?? internalCatalog;
  const universes = useUniverses(catalog);
  const label = universeLabel(value, universes) || fallbackLabel;

  useEffect(() => {
    if (!open) return;
    const dismiss = (event: MouseEvent) => {
      if (!wrapRef.current?.contains(event.target as Node)) setOpen(false);
    };
    const keydown = (event: KeyboardEvent) => {
      if (event.key === "Escape") setOpen(false);
    };
    document.addEventListener("mousedown", dismiss);
    document.addEventListener("keydown", keydown);
    return () => {
      document.removeEventListener("mousedown", dismiss);
      document.removeEventListener("keydown", keydown);
    };
  }, [open]);

  return (
    <div ref={wrapRef} className="lab-scope-popover-wrap">
      <button
        type="button"
        className={`lab-scope-symbols${label ? "" : " is-empty"}`}
        aria-label={t("快速选择品种")}
        aria-expanded={open}
        title={label || t("选择品种")}
        onClick={() => setOpen(previous => !previous)}
      >
        <span>{label || t("选择品种")}</span>
      </button>
      {open && (
        <div className="lab-scope-popover">
          <SymbolField
            context={context}
            value={value}
            minutes={minutes}
            allowExchanges={allowExchanges}
            catalog={catalog}
            inlineMenu
            onChange={onChange}
          />
        </div>
      )}
    </div>
  );
}
