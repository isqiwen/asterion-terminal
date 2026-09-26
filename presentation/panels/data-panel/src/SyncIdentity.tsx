import { useEffect, useState } from "react";
import type { RequestClient } from "@asterion/runtime-client/requests";

import { referenceVersions, type ReferenceVersion } from "./referenceVersions";

export function SyncIdentity({
  api,
  source,
  connection,
  exchange,
  disabled,
  value,
  onChange,
}: {
  api: RequestClient;
  source: string;
  connection?: string;
  exchange: string;
  disabled: boolean;
  value: string;
  onChange: (id: string) => void;
}) {
  const [versions, setVersions] = useState<ReferenceVersion[]>([]);
  const [error, setError] = useState("");
  const [revision, setRevision] = useState(0);
  useEffect(() => {
    let active = true;
    setVersions([]);
    setError("");
    async function load() {
      const items = await referenceVersions(
        api,
        "futures.contracts",
        source,
        connection,
        exchange,
        () => active,
      );
      if (active) setVersions(items);
    }
    void load().catch((e) => {
      if (active) setError(String(e));
    });
    return () => {
      active = false;
    };
  }, [api, source, connection, exchange, revision]);
  return (
    <div className="import-options">
      <label>
        合约依据
        <select
          aria-label="同步合约资料版本"
          value={value}
          disabled={disabled}
          required
          onChange={(e) => onChange(e.target.value)}
        >
          <option value="">请选择合约资料版本</option>
          {versions.map((item) => (
            <option key={item.id} value={item.id}>
              {new Date(item.created_at * 1000).toLocaleString()} ·{" "}
              {item.id.slice(0, 12)}
            </option>
          ))}
        </select>
      </label>
      <button
        type="button"
        disabled={disabled}
        onClick={() => {
          onChange("");
          setRevision((v) => v + 1);
        }}
      >
        刷新资料
      </button>
      <small>
        先同步同一连接的合约资料，或勾选研究数据准备。所选版本随本次同步固定保存。
      </small>
      {error && (
        <div role="alert" className="alert">
          {error}
        </div>
      )}
    </div>
  );
}
