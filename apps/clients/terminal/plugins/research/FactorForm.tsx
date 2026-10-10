import { useEffect, useState } from "react";
import {
  useWorkspaceDraft,
  useWorkspaceRequestId,
  useHistoryDatasets,
  DatasetPicker,
  ErrorNotice,
  type TerminalContext,
} from "../contract";
import { factorWords, t, type Run } from "./shared";

type Kind = keyof typeof factorWords;
const defaults = {
  lookback: "5",
  horizon: "1",
  evaluation: "full_sample",
  split: "",
  training: "",
  validation: "",
};

/**
 * What every factor task is given besides its series: windows, horizon and how
 * it is evaluated. `series` is what would be submitted, or null while nothing
 * is selected; `identity` changes when that selection does.
 */
function FactorParameters({
  kind,
  series,
  identity,
  observations,
  snapshot,
  busy,
  run,
}: Pick<TerminalContext, "snapshot" | "busy"> & {
  kind: Kind;
  series: Record<string, string> | null;
  identity: string;
  observations?: number;
  run: Run;
}) {
  const words = factorWords[kind];
  const taskService = snapshot?.task_service;
  const destination = JSON.stringify([snapshot?.data?.connection_id, taskService?.connection_id]);
  const [parameters, setParameters] = useWorkspaceDraft(`factor:${kind}`, defaults);
  const [submitted, setSubmitted] = useWorkspaceDraft(
    `submitted:${destination}:factor:${kind}`,
    "",
  );
  const [pendingId, setPendingId] = useWorkspaceRequestId(
    `factor-submission:${kind}`,
    JSON.stringify([destination, identity, parameters]),
  );
  async function submit(event: React.FormEvent) {
    event.preventDefault();
    const id = pendingId ?? `factor-${crypto.randomUUID()}`;
    setPendingId(id);
    const payload = {
      id,
      series,
      lookbacks: parameters.lookback.split(",").map(value => Number(value.trim())),
      horizon: Number(parameters.horizon),
      evaluation:
        parameters.evaluation === "full_sample"
          ? { mode: "full_sample" }
          : parameters.evaluation === "walk_forward"
            ? {
                mode: "walk_forward",
                training_events: Number(parameters.training),
                validation_events: Number(parameters.validation),
              }
            : { mode: "holdout", split_index: Number(parameters.split) },
    };
    if (await run("factor.submit", payload)) {
      setSubmitted(id);
      setPendingId(null);
    }
  }
  return (
    <>
      <form onSubmit={event => void submit(event)}>
        <fieldset disabled={busy}>
          <div className="research-fields">
            {(["lookback", "horizon"] as const).map(key => (
              <label key={key}>
                {t(words[key])}
                <input
                  aria-label={t(words[key])}
                  type={key === "lookback" ? "text" : "number"}
                  inputMode={key === "lookback" ? "text" : "numeric"}
                  pattern={
                    key === "lookback" ? "[ ]*[1-9][0-9]*[ ]*(,[ ]*[1-9][0-9]*[ ]*)*" : undefined
                  }
                  min="1"
                  max="10000"
                  step="1"
                  required
                  value={parameters[key]}
                  onChange={e => setParameters({ ...parameters, [key]: e.target.value })}
                />
              </label>
            ))}
          </div>
          <label>
            {t("评价方式")}
            <select
              aria-label={t("评价方式")}
              value={parameters.evaluation}
              onChange={e => setParameters({ ...parameters, evaluation: e.target.value })}
            >
              <option value="full_sample">{t("全样本评价")}</option>
              <option value="holdout">{t("时间留出评价")}</option>
              <option value="walk_forward">{t("滚动验证")}</option>
            </select>
          </label>
          {parameters.evaluation === "holdout" && (
            <label>
              {t(words.split)}
              <input
                aria-label={t(words.split)}
                type="number"
                min="1"
                max={observations ? observations - 1 : 19999}
                step="1"
                required
                value={parameters.split}
                onChange={e => setParameters({ ...parameters, split: e.target.value })}
              />
            </label>
          )}
          {parameters.evaluation === "walk_forward" && (
            <>
              <div className="research-fields">
                {(["training", "validation"] as const).map(key => (
                  <label key={key}>
                    {t(words[key])}
                    <input
                      aria-label={t(words[key])}
                      type="number"
                      min="1"
                      max="10000"
                      step="1"
                      required
                      value={parameters[key]}
                      onChange={e => setParameters({ ...parameters, [key]: e.target.value })}
                    />
                  </label>
                ))}
              </div>
              <p className="subtle">
                {t("固定训练窗口逐轮前移，验证段须完整覆盖剩余数据，共 2–16 轮。")}
              </p>
            </>
          )}
          <p className="subtle">
            {t("多个窗口按递增顺序用逗号分隔，最多 32 个，需使用留出或滚动验证。")}
          </p>
          <p className="subtle">{t("每段至少 30 个有效样本；跨分界标签不参与评价。")}</p>
          <div className="workflow-actions">
            <button
              className="primary"
              type="submit"
              disabled={!taskService?.online || !snapshot?.data?.online || !series}
            >
              {pendingId ? t("确认提交状态") : t("开始分析")}
            </button>
          </div>
        </fieldset>
      </form>
      {submitted && (
        <p className="subtle" role="status">
          {t("任务已提交，可在任务中心查看进度。")}
        </p>
      )}
      <details>
        <summary>{t("计算说明")}</summary>
        <p>
          {t(
            "动量只使用过去价格，留出段可使用此前历史预热。未来标签不进入因子，跨分界标签剔除。留出评价需事先固定参数；反复查看留出结果后调参不能证明样本外有效。",
          )}
        </p>
      </details>
    </>
  );
}

/** Momentum on one contract's bars, read from the selected dataset. */
export function BarFactorForm({
  snapshot,
  query,
  busy,
  trade,
  navigate,
  run,
}: Pick<TerminalContext, "snapshot" | "query" | "busy" | "trade" | "navigate"> & { run: Run }) {
  const datasets = snapshot?.datasets ?? [];
  // A factor studies exactly one contract.
  const data = datasets.length === 1 ? datasets[0] : undefined;
  return (
    <>
      <h3>{t("因子设置")}</h3>
      <DatasetPicker
        query={query}
        snapshot={snapshot}
        busy={busy}
        trade={trade}
        onDownload={() => navigate("workspace.data", { page: "history" })}
      />
      <p className="subtle">{t("单合约 · 按 K 线收盘价计算")}</p>
      {datasets.length > 1 && (
        <p role="alert" className="alert">
          {t("因子分析只分析一个合约，请只保留一个数据集。")}
        </p>
      )}
      <FactorParameters
        kind="bars"
        series={data ? { kind: "bars" } : null}
        identity={JSON.stringify(datasets.map(item => item.revision))}
        observations={data?.count}
        snapshot={snapshot}
        busy={busy}
        run={run}
      />
    </>
  );
}

/** Momentum on the daily closes of one version published in the archive. */
export function DailyFactorForm({
  snapshot,
  query,
  busy,
  navigate,
  run,
  workspaceParams,
}: Pick<TerminalContext, "query" | "snapshot" | "busy" | "navigate" | "workspaceParams"> & {
  run: Run;
}) {
  const dataConnection = snapshot?.data?.connection_id;
  const destination = JSON.stringify([dataConnection, snapshot?.task_service?.connection_id]);
  const [source, setSource] = useWorkspaceDraft(`daily-source:${destination}`, "");
  // A version handed over from the data page belongs to the service it was listed by.
  const matchesService =
    !workspaceParams?.source_dataset_id || workspaceParams.connection_id === dataConnection;
  useEffect(() => {
    if (workspaceParams?.source_dataset_id)
      setSource(matchesService ? workspaceParams.source_dataset_id : "");
  }, [workspaceParams, matchesService, setSource]);
  const navigationKey = JSON.stringify([destination, workspaceParams]);
  const [acknowledged, setAcknowledged] = useState("");
  const archive = useHistoryDatasets(snapshot, query);
  const sources = archive.items.filter(item => item.interval_minutes === 0);
  const selected = sources.find(item => item.id === source);
  return (
    <>
      {!matchesService && acknowledged !== navigationKey && (
        <p role="alert">{t("数据连接已改变，请重新选择当前服务中的日线。")}</p>
      )}
      <h3>{t("日线因子设置")}</h3>
      <p className="subtle">{t("按实际日线观测计算收盘价动量，不补齐缺失日期。")}</p>
      {archive.error && (
        <p role="alert">
          <ErrorNotice error={archive.error} />
        </p>
      )}
      {!archive.loading && !sources.length && <p>{t("当前数据服务没有已发布的日线数据。")}</p>}
      <button disabled={busy} onClick={() => navigate("workspace.data", { page: "history" })}>
        {t("下载历史日线")}
      </button>
      <label>
        {t("日线来源")}
        <select
          aria-label={t("日线来源")}
          disabled={busy}
          value={selected ? source : ""}
          onChange={event => {
            setSource(event.target.value);
            setAcknowledged(navigationKey);
          }}
        >
          <option value="">{t("选择日线数据版本")}</option>
          {sources.map(item => (
            <option key={item.id} value={item.id}>
              {item.contract_id} · {item.begin} — {item.end} · {item.source} · {item.id.slice(0, 8)}
            </option>
          ))}
        </select>
      </label>
      <FactorParameters
        kind="daily"
        series={selected ? { kind: "daily", dataset_id: selected.id } : null}
        identity={source}
        observations={selected?.rows}
        snapshot={snapshot}
        busy={busy}
        run={run}
      />
    </>
  );
}
