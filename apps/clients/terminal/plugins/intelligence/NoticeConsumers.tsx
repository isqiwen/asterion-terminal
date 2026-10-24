import { useEffect, useRef, useState } from "react";
import {
  ErrorNotice,
  asDisplayError,
  translate,
  type DisplayError,
  type TerminalContext,
} from "../contract";
import type { NoticeObject, NoticeConsumerPage } from "../../src/bridge/client";
const t = (key: string) => translate("asterion.terminal.intelligence", key);
const phases = {
  prepared: "已保留，待关系确认",
  confirmed: "引用关系已确认",
  released: "消费者已持久解除",
};
const same = (a: NoticeObject, b: NoticeObject) =>
  (["node", "service", "domain", "kind", "id", "version"] as const).every(key => a[key] === b[key]);
export function NoticeConsumers({
  context,
  source,
}: {
  context: TerminalContext;
  source: NoticeObject;
}) {
  const [page, setPage] = useState<NoticeConsumerPage | null>(null);
  const [pending, setPending] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  const epoch = useRef({ value: 0 });
  const sourceKey = JSON.stringify(source);
  const ownerKey = context.snapshot?.intelligence_service?.connection_id;
  useEffect(() => {
    const token = epoch.current;
    ++token.value;
    setPage(null);
    setError("");
    setPending(false);
    return () => {
      ++token.value;
    };
  }, [sourceKey, ownerKey]);
  async function load(after = "") {
    const current = ++epoch.current.value;
    setPending(true);
    setError("");
    try {
      const reply = await context.query("intelligence.consumers", { source, after });
      if (current !== epoch.current.value) return;
      const value = reply.notice_consumers;
      if (
        !value ||
        !same(value.source, source) ||
        value.items.some(item => !same(item.source, source))
      )
        throw Error(t("源引用登记响应不符，请重新读取"));
      setPage(value); // One bounded current page, not an unbounded aggregate or a fixed snapshot.
    } catch (failure) {
      if (current === epoch.current.value) {
        setPage(null);
        setError(asDisplayError(failure));
      }
    } finally {
      if (current === epoch.current.value) setPending(false);
    }
  }
  return (
    <section aria-label={t("已登记研究引用")}>
      <h4>{t("已登记研究引用")}</h4>
      <p>
        {t("这里显示源登记的当前页。准备和确认都保留引用；未知或断连不代表无引用，也不授权删除。")}
      </p>
      <button disabled={pending || !ownerKey} onClick={() => void load()}>
        {t("读取源引用登记")}
      </button>
      {pending && <p role="status">{t("正在读取源引用登记…")}</p>}
      {error && <ErrorNotice error={error} />}
      {page && (
        <>
          {!page.items.length && <p>{t("当前登记页为空，不能据此删除原证据。")}</p>}
          <table>
            <thead>
              <tr>
                <th>{t("研究所有者与任务")}</th>
                <th>{t("引用状态")}</th>
                <th>{t("使用范围")}</th>
                <th>{t("固定输入身份")}</th>
              </tr>
            </thead>
            <tbody>
              {page.items.map(item => (
                <tr key={JSON.stringify(item.consumer)}>
                  <td>
                    <code>
                      {item.consumer.node} / {item.consumer.service} / {item.consumer.id}
                    </code>
                  </td>
                  <td>{t(phases[item.phase])}</td>
                  <td>{item.use_scope}</td>
                  <td>
                    <code>{item.consumer.version}</code>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
          {page.next_after && (
            <button disabled={pending} onClick={() => void load(page.next_after)}>
              {t("下一页源引用")}
            </button>
          )}
        </>
      )}
    </section>
  );
}
