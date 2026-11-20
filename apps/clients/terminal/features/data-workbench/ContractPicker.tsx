import { useState } from "react";
import { warehouseName } from "../../src/ui/contract-names";
import { exchanges, t } from "./dataset";

// EXCHANGE/product/YYYY-MM → EXCHANGE/product; EXCHANGE/product stays as is.
export const productOf = (id: string) => id.split("/").slice(0, 2).join("/");
const isProduct = (id: string) => id.split("/").length === 2;
const rank = (exchange: string) => {
  const index = exchanges.indexOf(exchange);
  return index === -1 ? exchanges.length : index;
};
const normalizeCode = (raw: string) => {
  const parts = raw.trim().split("/");
  return parts.length >= 2
    ? [parts[0]!.toUpperCase(), parts[1]!.toLowerCase(), ...parts.slice(2)].join("/")
    : raw.trim();
};

// Minute acquisitions target whole products (all listed contracts in the span)
// and/or individual monthly contracts.
export function ContractPicker({
  products: knownProducts,
  known,
  focusExchange,
  value,
  onChange,
}: {
  products: string[];
  known: string[];
  focusExchange?: string;
  value: string[];
  onChange: (instruments: string[]) => void;
}) {
  const products = [
    ...new Set([...knownProducts, ...known.map(productOf), ...value.map(productOf)]),
  ]
    .filter(Boolean)
    .sort((a, b) => {
      const [ea = "", pa = ""] = a.split("/");
      const [eb = "", pb = ""] = b.split("/");
      return rank(ea) - rank(eb) || ea.localeCompare(eb) || pa.localeCompare(pb);
    });
  const [chosen, setChosen] = useState(
    () =>
      (value[0] && productOf(value[0])) ||
      (focusExchange && products.find(item => item.startsWith(`${focusExchange}/`))) ||
      products[0] ||
      "",
  );
  const [query, setQuery] = useState("");
  const candidate = normalizeCode(query);
  const covered = (id: string) =>
    value.includes(id) || (!isProduct(id) && value.includes(productOf(id)));
  const canAdd = /^[A-Z]+\/[a-z]{1,3}(\/\d{4}-\d{2})?$/.test(candidate) && !covered(candidate);
  const addCandidate = () => {
    if (!canAdd) return;
    onChange(
      isProduct(candidate)
        ? [...value.filter(item => productOf(item) !== candidate), candidate]
        : [...value, candidate],
    );
    setChosen(productOf(candidate));
    setQuery("");
  };

  const needle = query.trim().toUpperCase();
  const contractsByProduct = new Map<string, string[]>();
  for (const contract of [
    ...new Set([...known, ...value.filter(item => !isProduct(item))]),
  ].sort()) {
    const key = productOf(contract);
    const list = contractsByProduct.get(key);
    if (list) list.push(contract);
    else contractsByProduct.set(key, [contract]);
  }
  const filteredProducts = products.filter(item => {
    if (!needle) return true;
    const label = warehouseName(item) ?? "";
    if (`${item} ${label}`.toUpperCase().includes(needle)) return true;
    return (contractsByProduct.get(item) ?? []).some(c =>
      `${c} ${warehouseName(c) ?? ""}`.toUpperCase().includes(needle),
    );
  });
  const active = filteredProducts.includes(chosen) ? chosen : (filteredProducts[0] ?? "");
  const productMatchesNeedle =
    !needle || `${active} ${warehouseName(active) ?? ""}`.toUpperCase().includes(needle);
  const listed = (contractsByProduct.get(active) ?? []).filter(
    c => productMatchesNeedle || `${c} ${warehouseName(c) ?? ""}`.toUpperCase().includes(needle),
  );
  const wholeSelected = Boolean(active && value.includes(active));
  const pickedCount = active
    ? value.filter(item => !isProduct(item) && productOf(item) === active).length
    : 0;
  const toggle = (contract: string, on: boolean) =>
    onChange(on ? [...value, contract] : value.filter(item => item !== contract));

  return (
    <fieldset className="contract-picker">
      <legend>{t("品种或合约")}</legend>
      {value.length > 0 && (
        <div className="picker-selected">
          {value.map(item => {
            const label = warehouseName(item);
            const whole = isProduct(item);
            return (
              <button
                key={item}
                type="button"
                className="picker-chip"
                aria-label={t("移除 {item}", { item })}
                onClick={() => onChange(value.filter(entry => entry !== item))}
              >
                <code>{item}</code>
                {label && <span>{label}</span>}
                {whole && <small>{t("全部合约")}</small>}
                <span aria-hidden="true">×</span>
              </button>
            );
          })}
          <button type="button" className="ghost-action" onClick={() => onChange([])}>
            {t("清空")}
          </button>
        </div>
      )}
      <search className="picker-search">
        <input
          type="search"
          aria-label={t("搜索品种或输入代码，如 SHFE/rb、SHFE/rb/2027-01")}
          placeholder={t("搜索品种或输入代码，如 SHFE/rb、SHFE/rb/2027-01")}
          spellCheck={false}
          autoCorrect="off"
          autoCapitalize="none"
          value={query}
          onChange={event => setQuery(event.target.value)}
          onKeyDown={event => {
            if (event.key === "Enter" && !event.nativeEvent.isComposing) {
              event.preventDefault();
              addCandidate();
            }
          }}
        />
        <button type="button" disabled={!canAdd} onClick={addCandidate}>
          {t("添加")}
        </button>
      </search>
      {filteredProducts.length > 0 && (
        <div className="contract-picker-grid">
          <div className="contract-picker-products" role="group" aria-label={t("品种")}>
            {filteredProducts.map(item => {
              const label = warehouseName(item);
              const itemWhole = value.includes(item);
              const itemCount = value.filter(
                entry => !isProduct(entry) && productOf(entry) === item,
              ).length;
              return (
                <button
                  key={item}
                  type="button"
                  aria-pressed={active === item}
                  onClick={() => setChosen(item)}
                >
                  <span className="picker-product-main">
                    <code>{item}</code>
                    {label && <small>{label}</small>}
                  </span>
                  {(itemWhole || itemCount > 0) && (
                    <span className="picker-product-count">
                      {itemWhole ? t("全部") : itemCount}
                    </span>
                  )}
                </button>
              );
            })}
          </div>
          {active && (
            <div className="contract-picker-contracts">
              <label className="contract-picker-whole">
                <input
                  ref={element => {
                    if (element) element.indeterminate = !wholeSelected && pickedCount > 0;
                  }}
                  type="checkbox"
                  checked={wholeSelected}
                  onChange={event =>
                    onChange(
                      event.target.checked
                        ? [
                            ...value.filter(item => item !== active && productOf(item) !== active),
                            active,
                          ]
                        : value.filter(item => item !== active),
                    )
                  }
                />
                <span>
                  <strong>
                    {t("整个品种")} · {active}
                  </strong>
                  <small>{t("自动包含区间内全部在市合约")}</small>
                </span>
              </label>
              {listed.length > 0 && (
                <div className="contract-choices">
                  {listed.map(contract => {
                    const label = warehouseName(contract);
                    return (
                      <label key={contract} className="contract-choice">
                        <input
                          type="checkbox"
                          disabled={wholeSelected}
                          checked={wholeSelected || value.includes(contract)}
                          onChange={event => toggle(contract, event.target.checked)}
                        />
                        <code>{contract}</code>
                        {label && <small>{label}</small>}
                      </label>
                    );
                  })}
                </div>
              )}
            </div>
          )}
        </div>
      )}
    </fieldset>
  );
}
