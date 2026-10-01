import { useEffect, useState } from "react";
import { translate, type MessageValues } from "../i18n";
import type { DatasetSelection } from "../bridge/client";
const t = (key: string, values?: MessageValues) => translate("host", key, values);

// Margin and fee inputs shared by paper sessions and backtests.
export const costFields = [
  "margin_per_lot",
  "margin_rate",
  "open_fee",
  "close_today_fee",
  "close_yesterday_fee",
  "open_fee_rate",
  "close_today_fee_rate",
  "close_yesterday_fee_rate",
] as const;
export type CostValues = Record<(typeof costFields)[number], string>;
type Template = { values: CostValues; source: string; effective_from: string };

// A convenience for filling forms, kept per product in this browser profile.
// Sessions and tasks record the values they were created with.
const key = "asterion.cost-templates";
function readAll(): Record<string, Template> {
  try {
    const value = JSON.parse(localStorage.getItem(key) ?? "{}");
    return value && typeof value === "object" ? value : {};
  } catch {
    return {};
  }
}
function write(all: Record<string, Template>) {
  try {
    localStorage.setItem(key, JSON.stringify(all));
    window.dispatchEvent(new Event("asterion-cost-templates"));
  } catch {
    // Storage may be unavailable; templates are optional.
  }
}
// Saves rates from a recorded source, e.g. the broker's answer for an account.
export function saveCostTemplate(
  product: string,
  values: CostValues,
  source: string,
  effective_from: string,
) {
  write({ ...readAll(), [product]: { values, source, effective_from } });
}
function useTemplate(product: string) {
  const [template, setTemplate] = useState<Template | undefined>(() => readAll()[product]);
  useEffect(() => {
    const update = () => setTemplate(readAll()[product]);
    update();
    window.addEventListener("asterion-cost-templates", update);
    window.addEventListener("storage", update);
    return () => {
      window.removeEventListener("asterion-cost-templates", update);
      window.removeEventListener("storage", update);
    };
  }, [product]);
  return template;
}

export function CostTemplate({
  product,
  values,
  onApply,
  disabled = false,
}: {
  // "VENUE/product", e.g. "SHFE/rb"
  product: string;
  values: CostValues;
  onApply: (values: CostValues) => void;
  disabled?: boolean;
}) {
  const template = useTemplate(product);
  const [saving, setSaving] = useState(false);
  const [source, setSource] = useState("");
  const [effective, setEffective] = useState("");
  const complete = costFields.every(field => values[field] !== "");
  return (
    <section className="cost-template" aria-label={t("费率模板")}>
      <p>
        <strong>{t("费率模板")}</strong> · {product}{" "}
        {template ? (
          <span className="subtle">
            {t("来源 {source} · {date} 起生效", {
              source: template.source,
              date: template.effective_from,
            })}
          </span>
        ) : (
          <span className="subtle">{t("尚未保存")}</span>
        )}
      </p>
      <div className="source-actions">
        {template && (
          <button type="button" disabled={disabled} onClick={() => onApply(template.values)}>
            {t("填入模板")}
          </button>
        )}
        <button
          type="button"
          disabled={disabled || !complete}
          title={complete ? undefined : t("先填写全部保证金与手续费")}
          onClick={() => setSaving(!saving)}
        >
          {t("保存为模板")}
        </button>
      </div>
      {saving && (
        <div className="futures-fields">
          <label>
            {t("费率来源")}
            <input
              aria-label={t("费率来源")}
              value={source}
              placeholder={t("例如：期货公司费率表")}
              onChange={event => setSource(event.target.value)}
            />
          </label>
          <label>
            {t("生效日期")}
            <input
              aria-label={t("生效日期")}
              type="date"
              value={effective}
              onChange={event => setEffective(event.target.value)}
            />
          </label>
          <div className="source-actions">
            <button
              type="button"
              className="primary"
              disabled={!source.trim() || !effective}
              onClick={() => {
                const saved = Object.fromEntries(
                  costFields.map(field => [field, values[field]]),
                ) as CostValues;
                write({
                  ...readAll(),
                  [product]: { values: saved, source: source.trim(), effective_from: effective },
                });
                setSaving(false);
              }}
            >
              {t("确认保存")}
            </button>
          </div>
        </div>
      )}
    </section>
  );
}

// Cost drafts per portfolio contract, keyed by "VENUE.SYMBOL".
export type ContractCostDrafts = Record<string, CostValues>;
const blank: CostValues = {
  margin_per_lot: "",
  margin_rate: "0",
  open_fee: "",
  close_today_fee: "",
  close_yesterday_fee: "",
  open_fee_rate: "0",
  close_today_fee_rate: "0",
  close_yesterday_fee_rate: "0",
};
const labels: [(typeof costFields)[number], string][] = [
  ["margin_per_lot", "每手保证金"],
  ["margin_rate", "保证金率"],
  ["open_fee", "每手开仓费"],
  ["close_today_fee", "每手平今费"],
  ["close_yesterday_fee", "每手平昨费"],
  ["open_fee_rate", "开仓费率"],
  ["close_today_fee_rate", "平今费率"],
  ["close_yesterday_fee_rate", "平昨费率"],
];
const contractKey = (contract: { venue: string; symbol: string }) =>
  `${contract.venue}.${contract.symbol}`;
// The "contracts" entries of paper.create and research.submit, in selection order.
export function contractCostRequest(datasets: DatasetSelection[], drafts: ContractCostDrafts) {
  return datasets.map(dataset => ({
    venue: dataset.venue,
    symbol: dataset.symbol,
    ...(drafts[contractKey(dataset)] ?? blank),
  }));
}

// Margin and fee fields for every selected contract, each with its product's template.
export function ContractCosts({
  datasets,
  drafts,
  onChange,
  disabled = false,
}: {
  datasets: DatasetSelection[];
  drafts: ContractCostDrafts;
  onChange: (drafts: ContractCostDrafts) => void;
  disabled?: boolean;
}) {
  return (
    <>
      {datasets.map(dataset => {
        const key = contractKey(dataset);
        const values = drafts[key] ?? blank;
        const update = (next: CostValues) => onChange({ ...drafts, [key]: next });
        const name = `${dataset.venue} · ${dataset.symbol}`;
        return (
          <section
            className="account-field-group contract-costs"
            key={key}
            aria-label={t("{contract} 保证金与手续费", { contract: name })}
          >
            <h3>{t("{contract} 保证金与手续费", { contract: name })}</h3>
            <CostTemplate
              product={`${dataset.contract.venue}/${dataset.contract.product}`}
              values={values}
              onApply={applied => update({ ...values, ...applied })}
              disabled={disabled}
            />
            <div className="futures-fields">
              {labels.map(([field, label]) => (
                <label key={field}>
                  {t(label)}
                  <input
                    aria-label={t(label)}
                    inputMode="decimal"
                    value={values[field]}
                    onChange={event => update({ ...values, [field]: event.target.value })}
                    required
                  />
                </label>
              ))}
            </div>
          </section>
        );
      })}
    </>
  );
}
