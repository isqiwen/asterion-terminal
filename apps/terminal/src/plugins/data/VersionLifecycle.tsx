import type { RequestClient } from "../../api/requests";
import { useEffect, useRef, useState } from "react";

type Lifecycle = {
  version_id: string;
  archived: boolean;
  revision: number;
  is_latest: boolean;
  references: Record<string, number>;
  reference_count: number;
  protection_reason: string;
};
const labels: Record<string, string> = {
  data_lineage: "数据血缘",
  coverage_reports: "覆盖报告",
  research_runs: "研究运行（含排队与失败）",
  research_documents: "研究草稿与模板",
  research_packages: "已导入复现包",
  contract_rules: "合约规则版本",
};
export function VersionLifecycle({
  versionId,
  api,
  disabled,
}: {
  versionId: string;
  api: RequestClient;
  disabled: boolean;
}) {
  const [value, setValue] = useState<Lifecycle | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const live = useRef(true);
  useEffect(() => {
    live.current = true;
    return () => {
      live.current = false;
    };
  }, []);
  async function load() {
    setBusy(true);
    setError("");
    try {
      const result = await api.request<Lifecycle>(
        `/data/versions/${versionId}/lifecycle`,
      );
      if (live.current) setValue(result);
    } catch (e) {
      if (live.current) setError(String(e));
    } finally {
      if (live.current) setBusy(false);
    }
  }
  async function change() {
    if (!value) return;
    setBusy(true);
    setError("");
    try {
      const result = await api.request<Lifecycle>(
        `/data/versions/${versionId}/archive`,
        {
          archived: !value.archived,
          expected_revision: value.revision,
        },
      );
      if (live.current) setValue(result);
      window.dispatchEvent(new Event("asterion:catalog-changed"));
    } catch (e) {
      if (live.current) setError(String(e));
    } finally {
      if (live.current) setBusy(false);
    }
  }
  return (
    <details
      onToggle={(e) => {
        if (e.currentTarget.open && !value && !busy) void load();
      }}
    >
      <summary>引用保护与归档</summary>
      {busy && <p role="status">正在读取或更新版本状态…</p>}
      {value && (
        <section aria-label="版本引用保护">
          <p>
            <strong>{value.archived ? "已归档" : "未归档"}</strong> ·{" "}
            {value.is_latest ? "数据集最新版本" : "历史版本"} ·{" "}
            {value.reference_count} 项直接引用
          </p>
          <ul>
            {Object.entries(value.references)
              .filter(([, count]) => count > 0)
              .map(([kind, count]) => (
                <li key={kind}>
                  {labels[kind] || kind}：{count}
                </li>
              ))}
          </ul>
          <p>引用数量包含所有账户，仅展示汇总，不显示其他账户的研究内容。</p>
          <p>{value.protection_reason}</p>
          <p>
            归档保留数据及引用，不释放磁盘空间。归档最新版本会从默认目录隐藏该数据集，不自动显示旧版本；勾选“显示归档版本”可找回。历史读取、原输入重跑及同步累积继续使用原版本。
          </p>
          <button
            type="button"
            disabled={disabled || busy}
            onClick={() => void change()}
          >
            {value.archived ? "恢复此版本" : "归档此版本"}
          </button>
        </section>
      )}
      <button
        type="button"
        disabled={disabled || busy}
        onClick={() => void load()}
      >
        刷新引用与状态
      </button>
      {error && <p role="alert">{error}</p>}
    </details>
  );
}
