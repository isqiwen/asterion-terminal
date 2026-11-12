import { useState } from "react";
import { translate, listSeparator } from "../../src/i18n";

const t = (key: string) => translate("asterion.terminal.data-workbench", key);
// EXCHANGE/product/YYYY-MM → EXCHANGE/product
export const productOf = (contract: string) => contract.slice(0, contract.lastIndexOf("/"));

// The products and contracts one acquisition covers: a whole product, any of
// `known` by product, or products and contracts typed in that no list holds.
export function ContractPicker({
  known,
  value,
  onChange,
}: {
  known: string[];
  value: string[];
  onChange: (instruments: string[]) => void;
}) {
  const products = [...new Set(known.map(productOf))].sort();
  const [chosen, setChosen] = useState("");
  const [typed, setTyped] = useState("");
  const product = products.includes(chosen) ? chosen : (products[0] ?? "");
  const listed = known.filter(item => productOf(item) === product).sort();
  const toggle = (contract: string, on: boolean) =>
    onChange(on ? [...value, contract] : value.filter(item => item !== contract));
  return (
    <fieldset className="contract-picker">
      <legend>{t("品种或合约")}</legend>
      {products.length > 0 && (
        <div className="data-fields">
          <label>
            {t("品种")}
            <select value={product} onChange={event => setChosen(event.target.value)}>
              {products.map(item => (
                <option key={item}>{item}</option>
              ))}
            </select>
          </label>
          <button
            type="button"
            disabled={value.includes(product)}
            onClick={() =>
              onChange([...value.filter(item => productOf(item) !== product), product])
            }
          >
            {t("整个品种")}
          </button>
        </div>
      )}
      <div className="contract-choices">
        {listed.map(contract => (
          <label key={contract}>
            <input
              type="checkbox"
              disabled={value.includes(product)}
              checked={value.includes(contract) || value.includes(product)}
              onChange={event => toggle(contract, event.target.checked)}
            />
            {contract}
          </label>
        ))}
      </div>
      <div className="data-fields">
        <label>
          {t("其他品种或合约")}
          <input
            value={typed}
            placeholder="SHFE/rb, SHFE/rb/2027-01"
            onChange={event => setTyped(event.target.value.trim())}
          />
        </label>
        <button
          type="button"
          disabled={!/^[A-Z]+\/[a-z]{1,3}(\/\d{4}-\d{2})?$/.test(typed) || value.includes(typed)}
          onClick={() => {
            onChange([...value, typed]);
            setTyped("");
          }}
        >
          {t("添加")}
        </button>
      </div>
      {value.length > 0 && (
        <p className="subtle">
          {t("已选")}: {value.join(listSeparator())}{" "}
          <button type="button" onClick={() => onChange([])}>
            {t("清空")}
          </button>
        </p>
      )}
    </fieldset>
  );
}
