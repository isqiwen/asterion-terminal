import { useEffect, useState } from "react";
import { translate, type MessageValues } from "../i18n";
import type { CostVersion, DatasetSelection } from "../bridge/client";
const t = (key: string, values?: MessageValues) => translate("host", key, values);

// Margin and fee inputs of backtests.
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
const key = "asterion.cost-templates.v2";
function readAll(): Record<string, CostVersion[]> {
  const value: unknown = JSON.parse(localStorage.getItem(key) ?? "{}");
  if (!value || typeof value !== "object" || Array.isArray(value))
    throw new Error(t("费率模板格式无效，原数据未改动。"));
  for (const versions of Object.values(value)) {
    if (!Array.isArray(versions) || !versions.length || versions.length > 512)
      throw new Error(t("费率模板格式无效，原数据未改动。"));
    let previous = "";
    for (const row of versions) {
      if (!validVersion(row) || row.effective_from <= previous)
        throw new Error(t("费率模板格式无效，原数据未改动。"));
      previous = row.effective_from;
    }
  }
  return value as Record<string, CostVersion[]>;
}
function validVersion(row: CostVersion) {
  return (
    row &&
    typeof row.effective_from === "string" &&
    /^\d{4}-\d{2}-\d{2}$/.test(row.effective_from) &&
    Number.isFinite(Date.parse(row.effective_from)) &&
    new Date(row.effective_from).toISOString().slice(0, 10) === row.effective_from &&
    typeof row.source === "string" &&
    row.source.trim().length > 0 &&
    new TextEncoder().encode(row.source).length <= 256 &&
    row.values &&
    costFields.every(
      field =>
        typeof row.values[field] === "string" &&
        /^(0|[1-9]\d*)(\.\d{1,8})?$/.test(row.values[field]),
    )
  );
}
export function saveCostTemplate(
  product: string,
  values: CostValues,
  source: string,
  effective_from: string,
) {
  const version = { values: { ...values }, source: source.trim(), effective_from };
  if (!validVersion(version)) throw new Error(t("请填写有效的费率、来源和生效日期。"));
  const all = readAll(),
    versions = all[product] ?? [];
  if (versions.some(row => row.effective_from === effective_from))
    throw new Error(t("同一生效日期已存在费率版本，不会覆盖。"));
  if (versions.length >= 512) throw new Error(t("费率版本最多 512 个。"));
  all[product] = [...versions, version].sort((a, b) =>
    a.effective_from.localeCompare(b.effective_from),
  );
  localStorage.setItem(key, JSON.stringify(all));
  window.dispatchEvent(new Event("asterion-cost-templates"));
}
export function CostScheduleDetails({ versions }: { versions: CostVersion[] }) {
  return (
    <details>
      <summary>{t("已固定 {n} 个费率版本", { n: versions.length })}</summary>
      {versions.map(row => (
        <section key={row.effective_from} aria-label={row.effective_from}>
          <p>
            {t("来源 {source} · {date} 起生效", { source: row.source, date: row.effective_from })}
          </p>
          <dl className="experiment-fields">
            {labels.map(([field, label]) => (
              <div key={field}>
                <dt>{t(label)}</dt>
                <dd>{row.values[field]}</dd>
              </div>
            ))}
          </dl>
        </section>
      ))}
    </details>
  );
}
export function CostTemplate({
  product,
  values,
  onApply,
  disabled = false,
  firstDay,
}: {
  product: string;
  values: CostValues;
  firstDay: string;
  onApply: (values: CostValues, versions: CostVersion[]) => void;
  disabled?: boolean;
}) {
  const [versions, setVersions] = useState<CostVersion[]>([]);
  const [saving, setSaving] = useState(false),
    [source, setSource] = useState("");
  const [effective, setEffective] = useState(""),
    [error, setError] = useState("");
  useEffect(() => {
    const update = () => {
      try {
        setVersions(readAll()[product] ?? []);
        setError("");
      } catch (reason) {
        setVersions([]);
        setError(String(reason));
      }
    };
    update();
    window.addEventListener("asterion-cost-templates", update);
    window.addEventListener("storage", update);
    return () => {
      window.removeEventListener("asterion-cost-templates", update);
      window.removeEventListener("storage", update);
    };
  }, [product]);
  const selected = versions.filter(row => row.effective_from <= firstDay).at(-1);
  const complete = costFields.every(field => values[field] !== "");
  return (
    <section className="cost-template" aria-label={t("费率模板")}>
      <p>
        <strong>{t("费率模板")}</strong> · {product}
      </p>
      {!versions.length && <span className="subtle">{t("尚未保存")}</span>}
      {versions.length > 0 && <CostScheduleDetails versions={versions} />}
      {versions.length > 0 && !selected && (
        <p role="alert">{t("费率版本未覆盖首个交易日 {day}", { day: firstDay })}</p>
      )}
      <div className="source-actions">
        {versions.length > 0 && (
          <button
            type="button"
            disabled={disabled || !selected}
            onClick={() => selected && onApply(selected.values, structuredClone(versions))}
          >
            {t("填入模板")}
          </button>
        )}
        <button type="button" disabled={disabled || !complete} onClick={() => setSaving(!saving)}>
          {t("保存为模板")}
        </button>
      </div>
      {error && <p role="alert">{error}</p>}
      {saving && (
        <div className="futures-fields">
          <label>
            {t("费率来源")}
            <input
              aria-label={t("费率来源")}
              value={source}
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
          <button
            type="button"
            disabled={!source.trim() || !effective}
            onClick={() => {
              try {
                saveCostTemplate(
                  product,
                  Object.fromEntries(costFields.map(field => [field, values[field]])) as CostValues,
                  source,
                  effective,
                );
                setSaving(false);
              } catch (reason) {
                setError(String(reason));
              }
            }}
          >
            {t("确认保存")}
          </button>
        </div>
      )}
    </section>
  );
}

// Cost drafts per portfolio contract, keyed by "VENUE.SYMBOL".
export type ContractCostDrafts = Record<string, CostValues & { cost_schedule?: CostVersion[] }>;
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
// The "contracts" entries of research.submit, in selection order.
export function contractCostRequest(datasets: DatasetSelection[], drafts: ContractCostDrafts) {
  return datasets.map(dataset => ({
    venue: dataset.venue,
    symbol: dataset.symbol,
    cost_schedule: drafts[contractKey(dataset)]?.cost_schedule ?? [
      {
        effective_from: dataset.first_day,
        source: "User-entered fixed costs",
        values: Object.fromEntries(
          costFields.map(field => [field, (drafts[contractKey(dataset)] ?? blank)[field]]),
        ),
      },
    ],
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
        const schedule = drafts[key]?.cost_schedule;
        const active = schedule?.filter(row => row.effective_from <= dataset.first_day).at(-1);
        const values = schedule ? (active?.values ?? blank) : (drafts[key] ?? blank);
        const update = (next: CostValues & { cost_schedule?: CostVersion[] }) =>
          onChange({ ...drafts, [key]: next });
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
              firstDay={dataset.first_day}
              onApply={(applied, versions) => update({ ...applied, cost_schedule: versions })}
              disabled={disabled}
            />
            {schedule ? (
              <>
                <p>{t("按交易日使用固定的费率版本；更改模板不影响本次配置。")}</p>
                {!active && (
                  <p role="alert">
                    {t("费率版本未覆盖首个交易日 {day}", { day: dataset.first_day })}
                  </p>
                )}
                <CostScheduleDetails versions={schedule} />
                <button
                  type="button"
                  disabled={disabled}
                  onClick={() =>
                    update(
                      Object.fromEntries(
                        costFields.map(field => [field, values[field]]),
                      ) as CostValues,
                    )
                  }
                >
                  {t("改为固定费率")}
                </button>
              </>
            ) : (
              <p className="subtle">{t("固定费率假设：以下数值适用于全部所选交易日。")}</p>
            )}
            <div className="futures-fields">
              {labels.map(([field, label]) => (
                <label key={field}>
                  {t(label)}
                  <input
                    aria-label={t(label)}
                    inputMode="decimal"
                    value={values[field]}
                    onChange={event => update({ ...values, [field]: event.target.value })}
                    disabled={disabled || !!schedule}
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
