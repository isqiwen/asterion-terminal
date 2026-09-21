import { useEffect, useState } from "react";
import type { RequestClient } from "../../api/requests";

type Version = {
  id: string;
  dataset_id: string;
  created_at: number;
  archived?: boolean;
  manifest: {
    demo?: boolean;
    scope: { exchange?: string; connection_id?: string };
  };
};
type Page = { items: Version[]; total: number };

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
  const [versions, setVersions] = useState<Version[]>([]);
  const [error, setError] = useState("");
  const [revision, setRevision] = useState(0);
  useEffect(() => {
    let active = true;
    setVersions([]);
    setError("");
    async function load() {
      const items: Version[] = [];
      // Enumerate all pages; never silently omit an older selectable fixed version.
      for (let offset = 0; ; offset += 100) {
        if (!active) return;
        const page = await api.request<Page>(
          `/data/catalog?type_id=futures.contracts&layer=STANDARD&source=${encodeURIComponent(connection ?? source)}&limit=100&offset=${offset}`,
        );
        for (const dataset of page.items) {
          if (
            dataset.manifest.scope.exchange !== exchange ||
            dataset.manifest.scope.connection_id !== connection ||
            dataset.manifest.demo
          )
            continue;
          for (let historyOffset = 0; ; historyOffset += 100) {
            if (!active) return;
            const history = await api.request<Page>(
              `/data/catalog/${dataset.dataset_id}/versions?limit=100&offset=${historyOffset}`,
            );
            items.push(
              ...history.items.filter(
                (item) => !item.archived && !item.manifest.demo,
              ),
            );
            if (historyOffset + 100 >= history.total) break;
          }
        }
        if (offset + 100 >= page.total) break;
      }
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
