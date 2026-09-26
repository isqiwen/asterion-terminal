import type { RequestClient } from "@asterion/runtime-client/requests";
import { useRef, useState } from "react";

import type { Job } from "@asterion/ui-task-center/public";

type Checked = {
  id: string;
  status: string;
  detail: string;
  engine: string;
  strategy: { id: string; version: string; digest: string } | null;
  version_id: string;
  origin_run_id: string;
  includes_data: boolean;
  output_checksum: string;
  can_replay: boolean;
};
function download(name: string, content: BlobPart, type: string) {
  const url = URL.createObjectURL(new Blob([content], { type }));
  const link = document.createElement("a");
  link.href = url;
  link.download = name;
  link.click();
  setTimeout(() => URL.revokeObjectURL(url), 10000);
}
export function ResearchExport({
  runId,
  api,
  disabled,
}: {
  runId: string;
  api: RequestClient;
  disabled: boolean;
}) {
  const [include, setInclude] = useState(false),
    [busy, setBusy] = useState(false),
    [error, setError] = useState("");
  async function exportRun(kind: "package" | "results") {
    setBusy(true);
    setError("");
    try {
      if (kind === "package") {
        const content = await api.text(
          `/research/runs/${runId}/export?include_data=${include}`,
        );
        download(
          `research-${runId}.asterion.json`,
          content,
          "application/json;charset=utf-8",
        );
      } else {
        const value = await api.request<{
          filename: string;
          content_base64: string;
        }>(`/research/runs/${runId}/results-archive`);
        const bytes = Uint8Array.from(atob(value.content_base64), (c) =>
          c.charCodeAt(0),
        );
        download(value.filename, bytes, "application/zip");
      }
    } catch (e) {
      setError(String(e));
    } finally {
      setBusy(false);
    }
  }
  return (
    <details>
      <summary>导出结果与复现依据</summary>
      <button
        type="button"
        disabled={disabled || busy}
        onClick={() => void exportRun("results")}
      >
        导出结果 ZIP
      </button>
      <p>包含权益、回撤、成交、持仓 CSV 与回测报告。</p>
      <label className="research-ack">
        <input
          type="checkbox"
          checked={include}
          disabled={busy}
          onChange={(e) => setInclude(e.target.checked)}
        />
        附带回测所用行情（OHLC）
      </label>
      <button
        type="button"
        disabled={disabled || busy}
        onClick={() => void exportRun("package")}
      >
        导出复现包 JSON
      </button>
      <p>
        默认只导出结果、参数和固定版本引用；附带行情后可在缺少本机数据的终端核验并复现。
      </p>
      {error && <p role="alert">{error}</p>}
    </details>
  );
}
export function ResearchImport({
  api,
  accountEmail,
  disabled,
  onSubmitted,
}: {
  api: RequestClient;
  accountEmail: string;
  disabled: boolean;
  onSubmitted: (job: Job) => void;
}) {
  const [checked, setChecked] = useState<Checked | null>(null),
    [name, setName] = useState(""),
    [busy, setBusy] = useState(false),
    [error, setError] = useState(""),
    [ack, setAck] = useState(false);
  const sequence = useRef(0),
    command = useRef("");
  const scoped = (path: string) =>
    `${path}?expected_account=${encodeURIComponent(accountEmail)}`;
  async function read(file: File) {
    const current = ++sequence.current;
    setChecked(null);
    setAck(false);
    setName(file.name);
    setError("");
    setBusy(true);
    command.current = "";
    try {
      if (file.size > 8_000_000) throw new Error("复现包不能超过 8 MB");
      const value = JSON.parse(
        await api.text(scoped("/research/packages"), await file.text()),
      ) as Checked;
      if (current === sequence.current) setChecked(value);
    } catch (e) {
      if (current === sequence.current) setError(String(e));
    } finally {
      if (current === sequence.current) setBusy(false);
    }
  }
  async function recheck() {
    if (!checked) return;
    setBusy(true);
    setError("");
    setAck(false);
    try {
      setChecked(
        await api.request<Checked>(
          scoped(`/research/packages/${checked.id}/check`),
          {},
        ),
      );
    } catch (e) {
      setError(String(e));
      setChecked(null);
    } finally {
      setBusy(false);
    }
  }
  async function replay() {
    if (!checked?.can_replay || !ack) return;
    setBusy(true);
    setError("");
    if (!command.current) command.current = crypto.randomUUID();
    try {
      const job = await api.request<Job>(
        scoped(`/research/packages/${checked.id}/replay`),
        { command_id: command.current },
        60000,
      );
      onSubmitted(job);
      command.current = "";
      setAck(false);
    } catch (e) {
      setError(String(e));
      setAck(false);
    } finally {
      setBusy(false);
    }
  }
  const status: Record<string, string> = {
    READY: "可复现：重新计算与原结果一致",
    MISSING_DATA: "缺少本机固定行情",
    UNSUPPORTED: "不支持的引擎或策略版本",
    INVALID: "核验失败",
  };
  const resolution: Record<string, string> = {
    embedded: "使用包内行情",
    local_run: "使用本机已冻结的原运行行情",
    local_version: "使用本机固定数据版本",
  };
  return (
    <details>
      <summary>导入复现包</summary>
      <section aria-label="复现包核验">
        <label>
          选择复现包
          <input
            type="file"
            accept=".json,application/json"
            disabled={disabled || busy}
            onChange={(e) => {
              const file = e.target.files?.[0];
              if (file) void read(file);
              e.target.value = "";
            }}
          />
        </label>
        {name && <p>{name}</p>}
        {busy && <p>正在处理…</p>}
        {checked && (
          <>
            <p>
              <strong>{status[checked.status] || checked.status}</strong> ·{" "}
              {resolution[checked.detail] || checked.detail}
            </p>
            <p className="research-hash">
              {checked.engine} · {checked.strategy?.id} ·{" "}
              {checked.strategy?.version}
              <br />
              数据版本：{checked.version_id}
              <br />
              原运行：{checked.origin_run_id}
              <br />
              原结果校验和：{checked.output_checksum}
            </p>
            <p>
              校验一致不代表来源已认证。仅执行内置模型；包内行情只用于复现，不自动发布到数据目录。
            </p>
            <button
              type="button"
              disabled={disabled || busy}
              onClick={() => void recheck()}
            >
              重新核验复现包
            </button>
            <label className="research-ack">
              <input
                type="checkbox"
                disabled={!checked.can_replay || busy}
                checked={ack}
                onChange={(e) => setAck(e.target.checked)}
              />
              按包内固定输入与模型假设重跑
            </label>
            <button
              type="button"
              disabled={disabled || busy || !checked.can_replay || !ack}
              onClick={() => void replay()}
            >
              启动复现回测
            </button>
          </>
        )}
        {error && <p role="alert">{error}</p>}
      </section>
    </details>
  );
}
