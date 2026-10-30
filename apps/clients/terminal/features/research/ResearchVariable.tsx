import { useEffect, useRef, useState } from "react";
import type { ResourceObject, ResearchVariableDefinition } from "../../src/bridge/client";
import { ErrorNotice, asDisplayError, type DisplayError, type TerminalContext } from "../contract";
import { t, variableUnit } from "./shared";
export function ResearchVariablePicker({
  query,
  snapshot,
  reference,
  onChange,
  onResolved,
}: Pick<TerminalContext, "query" | "snapshot"> & {
  reference: ResourceObject | null;
  onChange: (value: ResourceObject | null) => void;
  onResolved: (value: ResearchVariableDefinition | null) => void;
}) {
  const [items, setItems] = useState<ResearchVariableDefinition[]>([]);
  const [error, setError] = useState<DisplayError>("");
  const ask = useRef(query);
  ask.current = query;
  const resolve = useRef(onResolved);
  resolve.current = onResolved;
  const destination = snapshot?.research_service?.connection_id;
  const identity = JSON.stringify(reference);
  useEffect(() => {
    let active = true;
    void ask
      .current("research.variable.list", { after: "", limit: 200 })
      .then(reply => {
        if (active) setItems(reply.research_variable?.definitions ?? []);
      })
      .catch(reason => {
        if (active) setError(asDisplayError(reason));
      });
    return () => {
      active = false;
    };
  }, [destination]);
  useEffect(() => {
    let active = true;
    resolve.current(null);
    setError("");
    const expected = JSON.parse(identity) as ResourceObject | null;
    if (expected)
      void ask
        .current("research.variable.read", { id: expected.id })
        .then(reply => {
          const value = reply.research_variable?.definition;
          if (
            !value ||
            Object.entries(expected).some(
              ([key, field]) => value.object[key as keyof ResourceObject] !== field,
            )
          )
            throw new Error(t("变量确认与原声明不一致"));
          if (active) resolve.current(value);
        })
        .catch(reason => {
          if (active) setError(asDisplayError(reason));
        });
    return () => {
      active = false;
    };
  }, [identity, destination]);
  return (
    <section aria-label={t("固定变量选择")}>
      <label>
        {t("固定变量选择")}
        <select
          value={reference?.id ?? ""}
          onChange={event => {
            const value = items.find(item => item.object.id === event.target.value);
            onChange(value?.object ?? null);
          }}
        >
          <option value="">{t("使用内置因子")}</option>
          {reference && !items.some(item => item.object.id === reference.id) && (
            <option value={reference.id}>{reference.id}</option>
          )}
          {items.map(item => (
            <option key={item.object.id} value={item.object.id}>
              {item.name} · {item.object.id} · {variableUnit(item.price_power)}
            </option>
          ))}
        </select>
      </label>
      {reference && (
        <code>
          {reference.id} · {reference.version}
        </code>
      )}
      {error && (
        <p role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
    </section>
  );
}
