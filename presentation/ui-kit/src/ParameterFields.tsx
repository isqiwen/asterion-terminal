/** Declarative controls shared by functional plugins; no business or API dependencies. */
export type ParameterValues = Record<string, number | string | boolean | null>;
type Named = { key: string; label: string };
export type ParameterDefinition = Named & (
  | { type: "integer"; minimum: number; maximum: number; default: number }
  | { type: "decimal"; minimum: string; maximum: string; scale: number; default: string }
  | { type: "boolean"; default: boolean }
  | { type: "enum"; choices: { value: string; label: string }[]; default: string }
);
export function ParameterFields({ fields, values, onChange }: {
  fields: ParameterDefinition[]; values: ParameterValues; onChange: (values: ParameterValues) => void;
}) {
  const update = (key: string, value: ParameterValues[string]) => onChange({ ...values, [key]: value });
  return <>{fields.map((p) => <label key={p.key}>{p.label}
    {p.type === "boolean" ? <input type="checkbox"
      checked={values[p.key] === true} onChange={(e) => update(p.key, e.target.checked)} />
      : p.type === "enum" ? <select aria-label={p.label} required value={typeof values[p.key] === "string" ? String(values[p.key]) : ""}
        onChange={(e) => update(p.key, e.target.value)}>
        <option value="">请选择</option>
        {typeof values[p.key] === "string" && !p.choices.some((c) => c.value === values[p.key]) &&
          <option value={String(values[p.key])}>不可用：{String(values[p.key])}</option>}
        {p.choices.map((c) => <option key={c.value} value={c.value}>{c.label}</option>)}
      </select> : p.type === "decimal" ? <input required type="text" inputMode="decimal"
        pattern={p.scale === 0 ? "-?(0|[1-9][0-9]*)" : `-?(0|[1-9][0-9]*)(\\.[0-9]{1,${p.scale}})?`}
        maxLength={40} title={`${p.minimum} 至 ${p.maximum}；最多 ${p.scale} 位小数`}
        value={typeof values[p.key] === "string" ? String(values[p.key]) : ""}
        onChange={(e) => update(p.key, e.target.value === "" ? null : e.target.value)} />
      : <input required type="number" step="1" min={p.minimum} max={p.maximum}
        value={typeof values[p.key] === "number" ? Number(values[p.key]) : ""}
        onChange={(e) => update(p.key, e.target.value === "" ? null : Number(e.target.value))} />}
  </label>)}</>;
}

function scaledDecimal(value: unknown, scale: number): bigint | null {
  if (typeof value !== "string" || value.length > 40 || !/^-?(0|[1-9][0-9]*)(\.[0-9]+)?$/.test(value)) return null;
  const negative = value.startsWith("-");
  const [whole, fraction = ""] = (negative ? value.slice(1) : value).split(".");
  if (fraction.length > scale) return null;
  return BigInt(whole + fraction.padEnd(scale, "0")) * (negative ? -1n : 1n);
}
export function parameterError(fields: ParameterDefinition[], values: ParameterValues): string {
  if (Object.keys(values).length !== fields.length || fields.some((p) => !(p.key in values)))
    return "请完整填写策略参数，不能包含未知字段。";
  for (const p of fields) {
    const value = values[p.key];
    let valid = false;
    if (p.type === "integer") valid = typeof value === "number" && Number.isSafeInteger(value) && value >= p.minimum && value <= p.maximum;
    else if (p.type === "boolean") valid = typeof value === "boolean";
    else if (p.type === "enum") valid = typeof value === "string" && p.choices.some((c) => c.value === value);
    else {
      const number = scaledDecimal(value, p.scale), minimum = scaledDecimal(p.minimum, p.scale), maximum = scaledDecimal(p.maximum, p.scale);
      valid = number !== null && minimum !== null && maximum !== null && number >= minimum && number <= maximum;
    }
    if (!valid) return `${p.label}：请按声明的类型、范围和精度填写。`;
  }
  return "";
}
