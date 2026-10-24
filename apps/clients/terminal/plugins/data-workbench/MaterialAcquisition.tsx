import { useEffect, useRef, useState } from "react";
import {
  ErrorNotice,
  asDisplayError,
  translate,
  type DisplayError,
  type TerminalContext,
} from "../contract";
import {
  taskOwner,
  type MaterialSource,
  type MaterialRead,
  type TaskRecord,
} from "../../src/bridge/client";
const t = (key: string) => translate("asterion.terminal.data-workbench", key);
const states: Record<TaskRecord["state"], string> = {
  queued: "排队中",
  running: "取得中",
  cancel_requested: "正在取消",
  publishing: "正在发布",
  succeeded: "已完成",
  failed: "失败",
  cancelled: "已取消",
  interrupted: "已中断",
};
export function MaterialAcquisition({ context }: { context: TerminalContext }) {
  const data = context.snapshot?.data;
  const [sources, setSources] = useState<MaterialSource[]>([]);
  const [sourceId, setSource] = useState("");
  const [id, setId] = useState(context.workspaceParams?.id ?? "");
  const [resource, setResource] = useState("");
  const [scope, setScope] = useState("");
  const [requests, setRequests] = useState("1");
  const [maximum, setMaximum] = useState("4194304");
  const [consent, setConsent] = useState(false);
  const [pending, setPending] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  const [status, setStatus] = useState<TaskRecord | null>(null);
  const [read, setRead] = useState<MaterialRead | null>(null);
  const epoch = useRef(0);
  const dataKey = data?.connection_id;
  const routeId = context.workspaceParams?.id;
  useEffect(() => {
    ++epoch.current;
    setSources([]);
    setSource("");
    setStatus(null);
    setRead(null);
    setConsent(false);
    setPending(false);
    setError("");
  }, [dataKey]);
  useEffect(() => {
    ++epoch.current;
    setId(routeId ?? "");
    setStatus(null);
    setRead(null);
    setConsent(false);
    setPending(false);
  }, [routeId]);
  useEffect(
    () => () => {
      ++epoch.current;
    },
    [],
  );
  const selected = sources.find(source => source.id === sourceId);
  const observed = data?.tasks.find(task => task.id === id && task.kind === "material_acquisition");
  const local = status?.id === id ? status : null;
  const task =
    local && (!observed || local.updated_at_ms >= observed.updated_at_ms) ? local : observed;
  const count = Number(requests),
    bytes = Number(maximum);
  const valid =
    !!selected &&
    !selected.credential_required &&
    selected.no_additional_fee &&
    !!id.trim() &&
    !!resource.trim() &&
    !!scope.trim() &&
    Number.isInteger(count) &&
    count > 0 &&
    count <= 32 &&
    Number.isInteger(bytes) &&
    bytes > 0 &&
    bytes <= selected.max_bytes;
  function changed() {
    ++epoch.current;
    setConsent(false);
    setRead(null);
    setPending(false);
  }
  async function run(action: (current: number) => Promise<void>) {
    const current = ++epoch.current;
    setPending(true);
    setError("");
    try {
      await action(current);
    } catch (reason) {
      if (current === epoch.current) setError(asDisplayError(reason));
    } finally {
      if (current === epoch.current) setPending(false);
    }
  }
  async function inspect(current: number) {
    const reply = await context.query("data.material.task", { id });
    if (
      !reply.material_task ||
      reply.material_task.id !== id ||
      reply.material_task.kind !== "material_acquisition"
    )
      throw Error(t("材料响应身份不符"));
    if (current === epoch.current) setStatus(reply.material_task);
  }
  return (
    <section aria-label={t("情报材料")}>
      <h3>{t("情报材料")}</h3>
      <p>{t("从已安装来源取得固定原件。取得、可读范围、语义提取和人工发布分别记录。")}</p>
      <p>
        {t("当前 Data")}：
        <code>
          {data?.node_id ?? "—"}/{data?.service ?? "—"}
        </code>
      </p>
      <button
        disabled={pending || !data?.online}
        onClick={() =>
          void run(async current => {
            const reply = await context.query("data.material.sources", {});
            if (!reply.material_sources) throw Error(t("材料来源响应缺失"));
            if (current !== epoch.current) return;
            setSources(reply.material_sources);
            const first = reply.material_sources.find(
              s => !s.credential_required && s.no_additional_fee,
            );
            setSource(first?.id ?? "");
            if (first) setMaximum(String(first.max_bytes));
            setConsent(false);
          })
        }
      >
        {t("刷新材料来源")}
      </button>
      {!sources.length && <p>{t("未安装可用的材料来源。")}</p>}
      {!!sources.length &&
        !sources.some(source => !source.credential_required && source.no_additional_fee) && (
          <p>{t("现有来源需要凭据或费用准入，当前取得路径不可用。")}</p>
        )}
      <label>
        {t("材料来源")}
        <select
          value={sourceId}
          disabled={pending}
          onChange={event => {
            changed();
            setSource(event.target.value);
            const next = sources.find(s => s.id === event.target.value);
            if (next) setMaximum(String(next.max_bytes));
          }}
        >
          <option value="">{t("选择来源")}</option>
          {sources.map(source => (
            <option
              key={source.id}
              value={source.id}
              disabled={source.credential_required || !source.no_additional_fee}
            >
              {source.name} · {source.id}
            </option>
          ))}
        </select>
      </label>
      {selected && (
        <>
          <p>{selected.usage}</p>
          <p>
            {t("来源工件")}：<code>{selected.provider_artifact}</code>
          </p>
          <p>{t("来源声明无额外费用；仍需核对材料使用权和使用范围。")}</p>
        </>
      )}
      <label>
        {t("材料操作 ID")}
        <input
          value={id}
          disabled={pending}
          onChange={event => {
            changed();
            setId(event.target.value);
            setStatus(null);
          }}
        />
      </label>
      <label>
        {t("来源资源标识")}
        <input
          value={resource}
          disabled={pending}
          maxLength={2048}
          onChange={event => {
            changed();
            setResource(event.target.value);
          }}
        />
      </label>
      <label>
        {t("本次使用范围")}
        <input
          value={scope}
          disabled={pending}
          maxLength={4096}
          onChange={event => {
            changed();
            setScope(event.target.value);
          }}
        />
      </label>
      <label>
        {t("外部请求上限")}
        <input
          type="number"
          min={1}
          max={32}
          value={requests}
          disabled={pending}
          onChange={event => {
            changed();
            setRequests(event.target.value);
          }}
        />
      </label>
      <label>
        {t("原件字节上限")}
        <input
          type="number"
          min={1}
          max={selected?.max_bytes ?? 4194304}
          value={maximum}
          disabled={pending}
          onChange={event => {
            changed();
            setMaximum(event.target.value);
          }}
        />
      </label>
      <label>
        <input
          type="checkbox"
          checked={consent}
          disabled={pending || !valid}
          onChange={event => setConsent(event.target.checked)}
        />
        {t("已核对来源使用范围与本次请求上限，确认取得原件。")}
      </label>
      <button
        disabled={pending || !data?.online || !valid || !consent}
        onClick={() =>
          void run(async current => {
            await context.trade("data.material.submit", {
              id,
              source_id: selected!.id,
              resource_id: resource,
              use_scope: scope,
              provider_artifact: selected!.provider_artifact,
              max_requests: count,
              max_bytes: bytes,
            });
            if (current === epoch.current) await inspect(current);
          })
        }
      >
        {t("接纳材料取得任务")}
      </button>
      <button disabled={pending || !data?.online || !id} onClick={() => void run(inspect)}>
        {t("读取材料任务")}
      </button>
      <button
        disabled={pending || !data?.online || !id}
        onClick={() =>
          void run(async current => {
            const reply = await context.query("data.material.read", { id });
            if (!reply.material_read || reply.material_read.receipt.reference.id !== id)
              throw Error(t("材料响应身份不符"));
            if (current === epoch.current) setRead(reply.material_read);
          })
        }
      >
        {t("读取固定材料")}
      </button>
      {task && (
        <div>
          <p>
            <code>{task.id}</code> · {t(states[task.state])} · {task.completed}/{task.total}
          </p>
          {task.error && <ErrorNotice error={task.error} />}
          {(task.state === "running" || task.state === "queued") && (
            <button
              disabled={pending || !data?.online}
              onClick={() =>
                void run(async current => {
                  await context.trade("task.action", {
                    owner: taskOwner(data, "data"),
                    id,
                    action: "cancel",
                  });
                  if (current === epoch.current) await inspect(current);
                })
              }
            >
              {t("取消材料取得")}
            </button>
          )}
        </div>
      )}
      {read && (
        <div>
          <p>{t("材料取得成功，尚未进行语义核验或情报发布。")}</p>
          <p>
            {t("取得时间")}：{read.receipt.acquired_at_ms} · {t("来源发布时间")}：
            {read.receipt.source_published_at_ms ?? t("未知")}
          </p>
          <p>
            {read.receipt.media_type} · {read.receipt.bytes} {t("字节")}
          </p>
          <p>
            {t("内容身份")}：<code>{read.receipt.content.version}</code>
          </p>
          {read.reading.state === "readable" ? (
            <>
              <p>
                {t("原始 UTF-8 字节范围")}：[0, {read.reading.readable_bytes})
              </p>
              <textarea readOnly rows={12} aria-label={t("固定材料正文")} value={read.text ?? ""} />
              <button
                disabled={pending}
                onClick={() =>
                  context.navigate("workspace.intelligence-notices", {
                    params: { material: read.receipt.reference.id },
                  })
                }
              >
                {t("人工核验此材料")}
              </button>
            </>
          ) : (
            <p>
              {t(
                read.reading.state === "invalid_utf8"
                  ? "原件不是受支持的 UTF-8 正文，取得记录仍保留。"
                  : "该材料格式尚不支持基础读取，取得记录仍保留。",
              )}
            </p>
          )}
        </div>
      )}
      <ErrorNotice error={error} />
    </section>
  );
}
