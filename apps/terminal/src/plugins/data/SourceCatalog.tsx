import { useEffect, useState } from "react";
import type { RequestClient } from "../../api/requests";
import type { components } from "../../api/schema";

type Catalog = components["schemas"]["ReferenceCatalog"];
type Release = components["schemas"]["ReferenceRelease"];
type Version = {
  id: string;
  created_at: number;
  manifest: { source: string; scope: { exchange?: string } };
};

export function SourceCatalog({
  api,
  connected,
  busy,
  setBusy,
  onPublished,
}: {
  api: RequestClient;
  connected: boolean;
  busy: boolean;
  setBusy: (value: boolean) => void;
  onPublished: (release: Release) => void;
}) {
  const [versions, setVersions] = useState<Version[]>([]);
  const [versionId, setVersionId] = useState("");
  const [symbols, setSymbols] = useState("");
  const [preview, setPreview] = useState<Catalog>();
  const [error, setError] = useState("");
  useEffect(() => {
    if (!connected) return;
    let live = true;
    api
      .request<{ items: Version[] }>(
        "/data/catalog?type_id=futures.contracts&layer=STANDARD&limit=100",
      )
      .then((value) => {
        if (live) setVersions(value.items);
      })
      .catch((e) => {
        if (live) setError(String(e));
      });
    return () => {
      live = false;
    };
  }, [api, connected]);
  async function run(publish: boolean) {
    setBusy(true);
    setError("");
    try {
      const request = {
        version_id: versionId,
        symbols: symbols.trim().split(/[\s,，]+/),
      };
      if (publish) {
        const release = await api.request<Release>(
          "/reference/source/publish",
          request,
        );
        setPreview(undefined);
        onPublished(release);
      } else {
        setPreview(undefined);
        setPreview(
          await api.request<Catalog>("/reference/source/preview", request),
        );
      }
    } catch (e) {
      setError(String(e));
    } finally {
      setBusy(false);
    }
  }
  return (
    <fieldset disabled={!connected || busy}>
      <legend>从同步资料创建目录</legend>
      <p className="panel-footnote">
        选择已同步的固定资料版本与来源代码。缺少完整交割年月或生命周期时无法发布。
      </p>
      <label>
        同步资料版本
        <select
          value={versions.some((v) => v.id === versionId) ? versionId : ""}
          onChange={(e) => {
            setVersionId(e.target.value);
            setPreview(undefined);
          }}
        >
          <option value="">选择合约资料</option>
          {versions.map((v) => (
            <option key={v.id} value={v.id}>
              {v.manifest.source} · {v.manifest.scope.exchange} ·{" "}
              {new Date(v.created_at * 1000).toLocaleString()} ·{" "}
              {v.id.slice(0, 8)}
            </option>
          ))}
        </select>
      </label>
      <label>
        固定资料版本 ID
        <input
          value={versionId}
          placeholder="也可填写历史版本 ID"
          onChange={(e) => {
            setVersionId(e.target.value);
            setPreview(undefined);
          }}
        />
      </label>
      <label>
        来源合约代码
        <input
          value={symbols}
          placeholder="例如 RB2505.SHF；多个代码以逗号分隔"
          onChange={(e) => {
            setSymbols(e.target.value);
            setPreview(undefined);
          }}
        />
      </label>
      <button
        disabled={!versionId || !symbols.trim()}
        onClick={() => void run(false)}
      >
        预览身份目录
      </button>
      {preview && (
        <div aria-label="身份目录预览">
          <p>
            {preview.products.length} 个品种 · {preview.contracts.length}{" "}
            个实际合约
          </p>
          <ul>
            {preview.contracts.map((c) => (
              <li key={c.id}>
                {c.id} · {c.listed_on} — {c.last_trade_on}
              </li>
            ))}
          </ul>
          <p className="panel-footnote">
            信息可用时间保留本次资料采集和入库依据，不代表历史当时已知。目录不替代交易时间与交易规则。
          </p>
          <button onClick={() => void run(true)}>发布身份目录</button>
        </div>
      )}
      {error && <p role="alert">{error}</p>}
    </fieldset>
  );
}
