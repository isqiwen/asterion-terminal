import { useEffect, useState } from "react";
import { parameterError, ParameterFields, type ParameterDefinition, type ParameterValues } from "@asterion/ui-kit/ParameterFields";
import type { RequestClient } from "@asterion/runtime-client/requests";

export type StrategyRef = { id: string; version: string; digest: string };
type Descriptor = {
  identity: StrategyRef;
  name: string;
  description: string;
  parameters: ParameterDefinition[];
};
export const sameStrategy = (a: StrategyRef | null, b: StrategyRef) =>
  a?.id === b.id && a.version === b.version && a.digest === b.digest;

export function StrategyPicker({
  api,
  connected,
  value,
  parameters,
  onChange,
  onReady,
}: {
  api: RequestClient;
  connected: boolean;
  value: StrategyRef | null;
  parameters: ParameterValues;
  onChange: (
    strategy: StrategyRef,
    parameters: ParameterValues,
  ) => void;
  onReady: (ready: boolean) => void;
}) {
  const [revision, setRevision] = useState(0);
  const [items, setItems] = useState<Descriptor[]>([]);
  const [error, setError] = useState("");
  const [loading, setLoading] = useState(false);
  useEffect(() => {
    let live = true;
    setItems([]);
    setError("");
    setLoading(connected);
    if (connected)
      api
        .request<Descriptor[]>("/research/strategies")
        .then((result) => {
          if (live) {
            setItems(result);
            setLoading(false);
          }
        })
        .catch((e) => {
          if (live) {
            setError(String(e));
            setLoading(false);
          }
        });
    return () => {
      live = false;
    };
  }, [api, connected, revision]);
  const selected = items.find((item) => sameStrategy(value, item.identity));
  const invalid = selected ? parameterError(selected.parameters, parameters) : "";
  useEffect(() => {
    onReady(connected && !!selected && !invalid);
  }, [connected, selected, invalid, onReady]);
  return (
    <section aria-label="策略配置">
      <label>
        策略
        <select
          required
          value={value ? `${value.id}:${value.digest}` : ""}
          onChange={(event) => {
            const item = items.find(
              (candidate) =>
                `${candidate.identity.id}:${candidate.identity.digest}` ===
                event.target.value,
            );
            if (item)
              onChange(
                item.identity,
                Object.fromEntries(
                  item.parameters.map((p) => [p.key, p.default]),
                ),
              );
          }}
        >
          <option value="">选择策略</option>
          {value && !selected && (
            <option value={`${value.id}:${value.digest}`}>
              不可用：{value.id} · {value.version}
            </option>
          )}
          {items.map((item) => (
            <option
              key={item.identity.id}
              value={`${item.identity.id}:${item.identity.digest}`}
            >
              {item.name} · {item.identity.version}
            </option>
          ))}
        </select>
      </label>
      <button type="button" disabled={!connected || loading}
        title="重新读取已启用的策略插件" onClick={() => setRevision((n) => n + 1)}>
        刷新策略
      </button>
      {value && !selected && !loading && !error && (
        <p role="alert">
          原策略未安装或实现已变化，请明确选择当前策略；原参数不会自动转换。
        </p>
      )}
      {loading && <p>正在读取策略…</p>}
      {error && <p role="alert">策略列表读取失败：{error}</p>}
      {selected && (
        <>
          <p>{selected.description}</p>
          {invalid && <p role="alert">{invalid}</p>}
          <div className="research-fields">
            <ParameterFields fields={selected.parameters} values={parameters}
              onChange={(values) => onChange(selected.identity, values)} />
          </div>
          <small title={selected.identity.digest}>
            实现摘要：{selected.identity.digest.slice(0, 12)}
          </small>
        </>
      )}
    </section>
  );
}
