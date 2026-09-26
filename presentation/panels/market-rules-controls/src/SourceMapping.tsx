import { useEffect, useState } from "react";
import type { RequestClient } from "@asterion/runtime-client/requests";
import type { components } from "@asterion/api-types/schema";
import type { ContractBasis } from "./public";

type Preview = {
  basis: ContractBasis;
  contract: components["schemas"]["Contract"];
  suggested_multiplier: string | null;
  multiplier_note: string;
  missing: string[];
};
type Version = {
  id: string;
  created_at: number;
  manifest: { scope: { exchange?: string; connection_id?: string } };
};

export function SourceMapping({
  api,
  disabled,
  contract,
  onApply,
}: {
  api: RequestClient;
  disabled: boolean;
  contract: string;
  onApply: (preview: Preview) => void;
}) {
  const [versions, setVersions] = useState<Version[]>([]);
  const [versionId, setVersionId] = useState("");
  const [target, setTarget] = useState(contract);
  const [preview, setPreview] = useState<Preview | null>(null);
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  useEffect(() => {
    if (disabled) return;
    let live = true;
    api
      .request<{ items: Version[] }>(
        "/data/catalog?type_id=futures.contracts&layer=STANDARD&limit=100",
      )
      .then((v) => {
        if (live) setVersions(v.items);
      })
      .catch((e) => {
        if (live) setError(String(e));
      });
    return () => {
      live = false;
    };
  }, [api, disabled]);
  async function inspect() {
    setBusy(true);
    setError("");
    setPreview(null);
    try {
      setPreview(
        await api.request<Preview>("/contract-rules/source/preview", {
          version_id: versionId,
          contract_id: target,
        }),
      );
    } catch (e) {
      setError(String(e));
    } finally {
      setBusy(false);
    }
  }
  return (
    <fieldset disabled={disabled || busy}>
      <legend>从合约资料创建</legend>
      <p>
        先在数据 → 数据同步选择数据源
        连接，同步“期货合约资料”。此处读取本机固定版本，不发起网络采集。
      </p>
      <label>
        合约资料版本
        <select
          value={versions.some((v) => v.id === versionId) ? versionId : ""}
          onChange={(e) => {
            setVersionId(e.target.value);
            setPreview(null);
          }}
        >
          <option value="">选择已同步的资料版本</option>
          {versions.map((v) => (
            <option key={v.id} value={v.id}>
              {v.manifest.scope.exchange} ·{" "}
              {new Date(v.created_at * 1000).toLocaleString()} ·{" "}
              {v.id.slice(0, 8)} ·{" "}
              {v.manifest.scope.connection_id ?? "提供方配置"}
            </option>
          ))}
        </select>
      </label>
      <label>
        资料版本 ID
        <input
          value={versionId}
          onChange={(e) => {
            setVersionId(e.target.value);
            setPreview(null);
          }}
          placeholder="也可填写历史版本 ID"
        />
      </label>
      <label>
        规范合约 ID
        <input
          value={target}
          onChange={(e) => {
            setTarget(e.target.value);
            setPreview(null);
          }}
          placeholder="例如 SHFE.RB.202610.20251016"
        />
      </label>
      <button
        type="button"
        disabled={!versionId || !target || disabled || busy}
        onClick={() => void inspect()}
      >
        读取合约资料
      </button>
      {!versions.length && (
        <p>没有可选资料，请先同步；已有历史版本可直接填写 ID。</p>
      )}
      {preview && (
        <div aria-label="来源映射预览">
          <p>
            {preview.basis.name} · {preview.basis.symbol} · 上市{" "}
            {preview.basis.listed} · 最后交易日{" "}
            {preview.basis.delisted || "未提供"}
          </p>
          <p>
            每手数量：{preview.basis.per_unit ?? "未提供"}{" "}
            {preview.basis.trade_unit}；报价单位：
            {preview.basis.quote_unit ?? "未提供"}；提供方乘数：
            {preview.basis.multiplier ?? "未提供"}
          </p>
          <p>
            最小报价说明：{preview.basis.quote_unit_desc ?? "未提供"}
            （请核实后填写数值）
          </p>
          <p>
            建议研究乘数：{preview.suggested_multiplier ?? "待填写"}。
            {preview.multiplier_note}
          </p>
          <p>
            仍须填写：{preview.missing.join("、")}
            。上市和最后交易日期不代表规则生效期间。
          </p>
          <button type="button" onClick={() => onApply(preview)}>
            采用资料并补全规则
          </button>
        </div>
      )}
      {error && <p role="alert">{error}</p>}
    </fieldset>
  );
}
