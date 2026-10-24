import { useEffect, useRef, useState } from "react";
import {
  taskOwner,
  type NoticeObject,
  type NoticeSourceObservation,
} from "../../src/bridge/client";
import { ErrorNotice, asDisplayError, type DisplayError, type TerminalContext } from "../contract";
import { t } from "./shared";
type FixedInput = {
  id: string;
  input_sha256: string;
  source: NoticeObject;
  owner: { node: string; service: string };
};
const same = (a: NoticeObject, b: NoticeObject) =>
  (["node", "service", "domain", "kind", "id", "version"] as const).every(k => a[k] === b[k]);
export function NoticeSourceChanges({
  context,
  input,
}: {
  context: TerminalContext;
  input: FixedInput;
}) {
  const [observation, setObservation] = useState<NoticeSourceObservation | null>(null);
  const [pending, setPending] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  const service = context.snapshot?.research_service;
  const epoch = useRef({ value: 0 });
  const identity = JSON.stringify(input);
  const connection = service?.connection_id;
  useEffect(() => {
    const token = epoch.current;
    ++token.value;
    setObservation(null);
    setPending(false);
    setError("");
    return () => {
      ++token.value;
    };
  }, [identity, connection]);
  async function inspect() {
    const current = ++epoch.current.value;
    setPending(true);
    setObservation(null);
    setError("");
    try {
      const reply = await context.query("notice.source.inspect", {
        id: input.id,
        owner: taskOwner(service, "research"),
      });
      if (current !== epoch.current.value) return;
      const value = reply.notice_source_observation;
      if (
        !value ||
        value.consumer.id !== input.id ||
        value.consumer.version !== input.input_sha256 ||
        value.consumer.node !== input.owner.node ||
        value.consumer.service !== input.owner.service ||
        !same(value.source, input.source)
      )
        throw Error(t("来源变化观察响应不符，请核对原任务"));
      setObservation(value);
    } catch (failure) {
      if (current === epoch.current.value) {
        setObservation(null);
        setError(asDisplayError(failure));
      }
    } finally {
      if (current === epoch.current.value) setPending(false);
    }
  }
  const view = observation?.current;
  return (
    <section aria-label={t("固定来源的后继变化")}>
      <h4>{t("固定来源的后继变化")}</h4>
      <p>{t("查询原任务固定的来源所有者，不改截止时间、输入或结果，不重新计算。")}</p>
      <button disabled={pending || !service?.online || context.busy} onClick={() => void inspect()}>
        {t("核查来源后继变化")}
      </button>
      {!!error && <p>{t("来源当前状态未知；失联不能解释为没有变化。")}</p>}
      {observation && view && (
        <div>
          <p>{t("这是最近一次当前观察，不是原历史结果或持续在线证明。")}</p>
          <p>
            {t("来源观察时间")}：{observation.observed_at_ms}
          </p>
          <p>
            {t(
              view.withdrawn
                ? "源事件当前已撤回"
                : view.latest_change
                  ? same(view.selected_publication, input.source)
                    ? "源事件有后继变化，当前指向原陈述"
                    : "源事件当前已更正"
                  : "当前未见已发布后继变化",
            )}
          </p>
          <p>
            {t("原固定发布")}：
            <code>
              {observation.source.id} / {observation.source.version}
            </code>
          </p>
          <p>
            {t("当前事件头")}：
            <code>
              {view.head.id} / {view.head.version}
            </code>
          </p>
          <p>
            {t("当前所选陈述")}：
            <code>
              {view.selected_publication.id} / {view.selected_publication.version}
            </code>
          </p>
          {view.latest_change && (
            <p>
              {t("后继变化理由")}：{view.latest_change.reason}
            </p>
          )}
          {view.pending_change && (
            <p>
              {t("源事件有未完成变化，尚未发布")}：{view.pending_change.reason}
            </p>
          )}
        </div>
      )}
      <ErrorNotice error={error} />
    </section>
  );
}
