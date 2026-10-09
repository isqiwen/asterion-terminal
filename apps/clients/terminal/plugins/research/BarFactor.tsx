import {
  useWorkspaceDraft,
  useWorkspaceRequestId,
  DatasetPicker,
  asDisplayError,
  type DisplayError,
  type TerminalContext,
} from "../contract";
import { t, type Run } from "./shared";

/** Momentum on one contract's bars, read from the selected dataset. */
export function BarFactorForm({
  snapshot,
  query,
  busy,
  trade,
  navigate,
  run,
  setError,
}: Pick<TerminalContext, "snapshot" | "query" | "busy" | "trade" | "navigate"> & {
  run: Run;
  setError: (error: DisplayError) => void;
}) {
  const taskService = snapshot?.task_service;
  const datasets = snapshot?.datasets ?? [];
  // Factor analysis studies exactly one contract.
  const data = datasets.length === 1 ? datasets[0] : undefined;
  const [parameters, setParameters] = useWorkspaceDraft("factor", {
    lookback: "5",
    horizon: "1",
    evaluation: "full_sample",
    split: "",
    training: "",
    validation: "",
  });
  const destination = JSON.stringify([snapshot?.data?.connection_id, taskService?.connection_id]);
  const [submitted, setSubmitted] = useWorkspaceDraft(`submitted:${destination}:factor`, "");
  const [pendingId, setPendingId] = useWorkspaceRequestId(
    "factor-submission",
    JSON.stringify([destination, parameters, datasets.map(item => item.revision)]),
  );
  async function submit(event: React.FormEvent) {
    event.preventDefault();
    const id = pendingId ?? `factor-${crypto.randomUUID()}`;
    setPendingId(id);
    try {
      const payload = {
        id,
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
    } catch (reason) {
      setError(asDisplayError(reason));
    }
  }
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
      <form onSubmit={event => void submit(event)}>
        <fieldset disabled={busy || !data}>
          <div className="research-fields">
            {(
              [
                ["lookback", "回看 K 线数"],
                ["horizon", "未来收益 K 线数"],
              ] as const
            ).map(([key, label]) => (
              <label key={key}>
                {t(label)}
                <input
                  aria-label={t(label)}
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
              {t("前段 K 线数")}
              <input
                aria-label={t("前段 K 线数")}
                type="number"
                min="1"
                max={data ? data.count - 1 : 9999}
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
                {(
                  [
                    ["training", "训练 K 线数"],
                    ["validation", "每轮验证 K 线数"],
                  ] as const
                ).map(([key, label]) => (
                  <label key={key}>
                    {t(label)}
                    <input
                      aria-label={t(label)}
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
              disabled={!taskService?.online || !snapshot?.data?.online || !data}
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
