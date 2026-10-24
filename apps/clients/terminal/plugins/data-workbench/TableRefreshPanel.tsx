import { AcquisitionRetirement } from "./AcquisitionRetirement";
import { taskOwner } from "../../src/bridge/client";
import { useEffect, useRef, useState } from "react";
import {
  type TerminalContext,
  translate,
  ErrorNotice,
  BackendError,
  asDisplayError,
  type DisplayError,
  getLocale,
} from "../contract";
import {
  savedCredential,
  type TableSource,
  type TableRevision,
  type TableRefreshPlan,
  type TableUpdateResult,
} from "../../src/bridge/client";
const t = (key: string) => translate("asterion.terminal.data-workbench", key);
export function TableRefreshPanel({
  context,
  source,
  revision,
  onReadCurrent,
}: {
  context: TerminalContext;
  source: TableSource;
  revision: TableRevision;
  onReadCurrent: () => void;
}) {
  const [plan, setPlan] = useState<TableRefreshPlan | null>(null);
  const [id, setId] = useState("");
  const [accepted, setAccepted] = useState(false);
  const [working, setWorking] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  const [token, setToken] = useState("");
  const [receipt, setReceipt] = useState<TableUpdateResult | null>(null);
  const connection = context.snapshot?.data?.sources.find(
    item => item.plugin_id === source.plugin_id,
  );
  const publicMargins = source.plugin_id === "asterion.data.shfe";
  const saved = savedCredential(context.snapshot, connection?.id ?? "");
  const [rate, setRate] = useState(
    String(saved?.requests_per_minute ?? connection?.connection?.requests_per_minute_default ?? 60),
  );
  const generation = useRef(0);
  useEffect(
    () => () => {
      ++generation.current;
    },
    [],
  );
  const tasks = (context.snapshot?.data?.tasks ?? []).filter(
    task =>
      task.table_refresh?.source === revision.source &&
      task.table_refresh.partition.table === revision.table &&
      task.table_refresh.partition.exchange === revision.exchange &&
      task.table_refresh.partition.period === revision.period,
  );
  const task = id
    ? tasks.find(item => item.id === id)
    : tasks.reduce<(typeof tasks)[number] | undefined>(
        (last, item) =>
          !last || item.submission_sequence > last.submission_sequence ? item : last,
        undefined,
      );
  const running = !!task && ["queued", "running", "cancel_requested"].includes(task.state);
  const unsupportedVenue = !source.venues.includes(revision.exchange);
  const enabled =
    !unsupportedVenue && !!context.snapshot?.data?.online && (!!connection || publicMargins);
  const executionReady = enabled;
  const dataConnection = context.snapshot?.data?.connection_id;
  const owner = useRef(context.snapshot?.data);
  owner.current = context.snapshot?.data;
  const read = useRef(context.query);
  read.current = context.query;
  useEffect(() => {
    let active = true;
    setReceipt(null);
    if (task?.state === "succeeded")
      void read
        .current("task.result.read", { id: task.id, owner: taskOwner(owner.current, "data") })
        .then(reply => {
          if (active && reply.task_result?.kind === "table_update")
            setReceipt(reply.task_result.result);
        })
        .catch(reason => {
          if (active) setError(asDisplayError(reason));
        });
    return () => {
      active = false;
    };
  }, [task?.id, task?.state, task?.result_digest, dataConnection]);
  async function act(action: () => Promise<void>) {
    const ticket = ++generation.current;
    setWorking(true);
    setError("");
    try {
      await action();
    } catch (reason) {
      if (ticket === generation.current) setError(asDisplayError(reason));
    } finally {
      if (ticket === generation.current) setWorking(false);
    }
  }
  function preview() {
    void act(async () => {
      const ticket = generation.current;
      setPlan(null);
      setAccepted(false);
      const reply = await context.query("data.tables.refresh.plan", {
        provider: source.plugin_id,
        source: revision.source,
        table: revision.table,
        exchange: revision.exchange,
        period: revision.period,
        previous_snapshot: revision.snapshot,
        requests_per_minute: Number(saved?.requests_per_minute ?? rate),
      });
      if (ticket !== generation.current) return;
      if (!reply.table_refresh_plan) throw new Error(t("重新获取预览不可用"));
      setPlan(reply.table_refresh_plan);
      setId(`refresh-${crypto.randomUUID()}`);
    });
  }
  function submit() {
    if (!plan) return;
    void act(async () => {
      const ticket = generation.current;
      try {
        await context.trade("data.tables.refresh.submit", {
          id,
          connection_id: context.snapshot?.data?.connection_id ?? "",
          query: plan.query,
          plan_id: plan.id,
          token: publicMargins || saved ? "" : token,
        });
        if (ticket === generation.current) setAccepted(true);
      } finally {
        setToken("");
      }
    });
  }
  const submitted = accepted || (!!id && !!task);
  return (
    <section className="history-update" aria-label={t("重新获取此日分区")}>
      <h4>{t("重新获取此日分区")}</h4>
      <p>
        {revision.table} · {revision.exchange} · {revision.period} · {revision.source}
      </p>
      <p className="subtle">
        {t(
          "只重新获取所选已入库日分区。来源更正产生新修订；相同内容不新增修订，旧快照和旧实验保持不变。",
        )}
      </p>
      <p className="subtle">
        {t("来源声明支持的交易所")}：{source.venues.join(" · ")}
      </p>
      {unsupportedVenue ? (
        <p role="alert">
          {t(
            "当前来源声明不支持所选交易所。请核对来源选择或启用明确支持该交易所的插件；不会提交此次请求。这不说明其他来源或日期的历史覆盖。",
          )}
        </p>
      ) : (
        <p className="subtle">
          {t(
            "所选日期的来源历史覆盖：未知。当前接口没有提供可核实的历史日期边界；支持该表和交易所不等于覆盖该日期。",
          )}
        </p>
      )}
      {!task && !accepted && (
        <p className="subtle">{t("本次尚未提交重新获取；预览也不会请求来源。")}</p>
      )}
      {!plan && (
        <>
          <label>
            {t("每分钟请求数")}
            <input
              type="number"
              min="1"
              max={publicMargins ? 60 : (connection?.max_requests_per_minute ?? 500)}
              value={rate}
              disabled={working || running || !!saved}
              onChange={event => setRate(event.target.value)}
            />
          </label>
          <button disabled={!enabled || working || running || context.busy} onClick={preview}>
            {t("预览重新获取")}
          </button>
        </>
      )}
      {plan && (
        <div className="history-update-preview">
          <p>{t("本次仅重取一个日分区，实际请求次数由数据源决定，受共享额度约束。")}</p>
          <p>
            {plan.query.table} · {plan.query.exchange} · {plan.query.period} · {plan.query.provider}
          </p>
          <p>
            {t("原分区修订")}：<code>{plan.previous.snapshot}</code>
          </p>
          <p>
            {t("原内容摘要")}：<code>{plan.previous.content}</code>
          </p>
          <p>
            {t("每分钟请求数")}：{plan.query.requests_per_minute}
          </p>
          <p>
            {t(
              plan.query.table === "futures.daily" && plan.query.exchange === "SHFE"
                ? "独立观测预览：成功且通过既有原文安全检查的上期所日线响应，即使内容未变也保存本次观测；旧修订和旧导入原文不回写。"
                : "本期独立观测仅覆盖通过既有安全检查的上期所成功日线响应；此选择不承诺留存观测。",
            )}
          </p>
          <p className="subtle">
            {t(
              "不会批量回补；每次仍须确认。观测接收时间属于本次请求，不是所选历史修订首次取得时间。",
            )}
          </p>
          {!submitted && (
            <>
              <p>{t("预览未下载数据；仅显式确认后通过既有授权提交任务。")}</p>
              {!saved && connection?.credential_required && (
                <label>
                  {connection.connection
                    ? getLocale() === "zh-CN"
                      ? connection.connection.credential_label_zh
                      : connection.connection.credential_label_en
                    : t("数据源凭据")}
                  <input
                    type="password"
                    autoComplete="off"
                    value={token}
                    disabled={working}
                    onChange={event => setToken(event.target.value)}
                  />
                </label>
              )}
              <button
                disabled={working || context.busy}
                onClick={() =>
                  void act(() =>
                    context.trade("data.download.budget.configure", {
                      source: publicMargins ? source.source : (connection?.id ?? ""),
                      ...(publicMargins ? { provider: source.plugin_id } : {}),
                      token: publicMargins || saved ? "" : token,
                      requests_per_minute: plan.query.requests_per_minute,
                    }),
                  )
                }
              >
                {t("应用共享额度")}
              </button>
              <button
                disabled={
                  !executionReady ||
                  working ||
                  context.busy ||
                  (!!connection?.credential_required && !saved && !token)
                }
                onClick={submit}
              >
                {t("确认重新获取此分区")}
              </button>
              <button
                disabled={working}
                onClick={() => {
                  setPlan(null);
                  setToken("");
                }}
              >
                {t("取消预览")}
              </button>
            </>
          )}
          {submitted && (
            <p role="status">
              {t("重新获取任务已提交")}: {id}
            </p>
          )}
        </div>
      )}
      {task && (
        <div aria-live="polite">
          <AcquisitionRetirement
            key={`retire:${owner.current?.connection_id}:${task.id}`}
            id={task.id}
            owner={taskOwner(owner.current, "data")}
            busy={working || context.busy || !owner.current?.online}
            query={context.query}
            trade={context.trade}
          />
          <p>
            {t("任务")} {task.id} · {t("执行次数")} {task.attempt} · {task.state} · {task.completed}
            %
          </p>
          {running && (
            <button
              disabled={working || context.busy || task.state === "cancel_requested"}
              onClick={() =>
                void act(() =>
                  context.trade("task.action", {
                    id: task.id,
                    action: "cancel",
                    owner: taskOwner(owner.current, "data"),
                  }),
                )
              }
            >
              {t("停止重新获取")}
            </button>
          )}
          {task.state === "failed" && (
            <p role="status">
              {t(
                task.error_code === "permission_denied"
                  ? "来源拒绝访问。请核对来源授权或账户权限后再显式重试；这不能证明所选日期不在历史覆盖内。"
                  : task.error_code === "unavailable" || task.error_code === "resource_exhausted"
                    ? "本次请求未完成，来源不可用或资源受限。请核对来源状态及共享额度，条件恢复后可显式重试同一固定任务；不会自动重试，也不能据此判断历史覆盖。"
                    : task.error_code === "conflict"
                      ? "固定目标或服务状态发生冲突。请读取当前分区并重新预览；不会用旧目标覆盖其他更正。"
                      : "本次请求失败，现有证据不能确定是否为临时故障或历史覆盖问题。请核对下方原因；目标或来源已变化时读取当前分区并重新预览，否则可显式重试同一固定任务。",
              )}
            </p>
          )}
          {["cancelled", "interrupted"].includes(task.state) && (
            <p role="status">
              {t(
                "执行已停止，未完成不代表来源没有数据。可以显式重试原固定任务；已发布修订保留，目标变化时先读取当前分区并重新预览。",
              )}
            </p>
          )}
          {["failed", "cancelled", "interrupted"].includes(task.state) && (
            <>
              <p role="alert">
                {t(
                  "本次未完成，已发布的修订不会回退。可以显式重试同一固定目标；目标有其他更正时请重新读取并预览。",
                )}
              </p>
              {task.error && (
                <p>
                  <ErrorNotice
                    error={new BackendError(task.error_code ?? "operation_failed", task.error)}
                  />
                </p>
              )}
              <button
                disabled={working || context.busy || task.retired}
                onClick={() =>
                  void act(() =>
                    context.trade("task.action", {
                      id: task.id,
                      action: "retry",
                      owner: taskOwner(owner.current, "data"),
                    }),
                  )
                }
              >
                {t("重试此任务")}
              </button>
            </>
          )}
          {task.state === "succeeded" && receipt && (
            <>
              <p role="status">
                {receipt.stored
                  ? t("重新获取完成：产生新修订")
                  : receipt.unchanged
                    ? t("重新获取完成：内容未变，未新增修订")
                    : t("数据源未返回可发布数据，旧内容保留")}
              </p>
              {receipt.refresh_observation && (
                <p>
                  {t("本次独立观测已保存")}：<code>{receipt.refresh_observation.id}</code>
                </p>
              )}
              {!!receipt.unpublished && (
                <p>
                  {t(
                    "本次来源响应为空，原因与历史覆盖均未知；不能据此认定无交易、数据不存在或来源不覆盖。旧内容未删除。可先核实来源公布情况，之后读取当前分区并重新预览，显式发起新任务。",
                  )}
                </p>
              )}
              <p>
                {t("原分区修订")}：<code>{task.table_refresh?.previous_snapshot}</code>
              </p>
              {receipt.refreshed_revision && (
                <>
                  <p>
                    {t("取得的分区修订")}：<code>{receipt.refreshed_revision.snapshot}</code>
                  </p>
                  <p>
                    {t("取得的内容摘要")}：<code>{receipt.refreshed_revision.content}</code>
                  </p>
                </>
              )}
            </>
          )}
        </div>
      )}
      {task && ["succeeded", "failed", "cancelled", "interrupted"].includes(task.state) && (
        <button
          disabled={working}
          onClick={() => {
            setPlan(null);
            setId("");
            setAccepted(false);
            onReadCurrent();
          }}
        >
          {t("读取当前分区")}
        </button>
      )}
      {error && (
        <p role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
    </section>
  );
}
