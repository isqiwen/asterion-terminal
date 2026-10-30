import type { TableRefreshObservation } from "../../src/bridge/client";
import { translate } from "../contract";
const t = (key: string) => translate("asterion.terminal.data-workbench", key);
export function RefreshObservation({
  observation: o,
}: {
  observation: TableRefreshObservation | null | undefined;
}) {
  return (
    <details>
      <summary>{t("独立重取观测")}</summary>
      {o ? (
        <section aria-label={t("独立重取观测")}>
          <p>
            {t(
              "这是显式重取后取得的独立响应证据，不是旧修订首次导入原文；不会改变旧快照或旧实验。",
            )}
          </p>
          <p>
            {t("观测身份")} <code>{o.id}</code>
          </p>
          <p>
            {t("操作身份")} <code>{o.operation}</code>
          </p>
          <p>
            {o.task_id} · {t("执行次数")} {o.attempt} · {o.provider} · {o.revision.source}
          </p>
          <p>
            {t("本次响应接收时间")} {o.received_at_ns} ns · {t("观测落盘时间")} {o.recorded_at_ns}{" "}
            ns
          </p>
          <p>
            {t(
              o.content_changed_from_selected
                ? "规范化内容不同于预览所选修订"
                : "规范化内容与预览所选修订相同",
            )}
          </p>
          <p>
            {t("预览所选修订")} <code>{o.selected_snapshot}</code>
          </p>
          <p>
            {t("响应对应的保存修订")} <code>{o.revision.snapshot}</code>
          </p>
          <p>
            {t("规范化内容摘要")} <code>{o.revision.content}</code> · {t("规范化对象摘要")}{" "}
            <code>{o.normalized_object}</code>
          </p>
          <p>
            {t("本次来源实现摘要")} <code>{o.provider_artifact}</code>
          </p>
          <p>
            {o.format} · {o.response_bytes} bytes · SHA-256 <code>{o.response_sha256}</code>
          </p>
          <p className="subtle">
            {t(
              "仅显示此分区已保存时间最近的观测；任务收据保留各次观测。原始导入质量与本次观测不是同一证据。",
            )}
          </p>
          <p className="subtle">
            {t("对应仅指同批交付的规范化内容，不表示所有原文字段一致或行情真实性已经验证。")}
          </p>
          <pre
            aria-label={t("独立观测响应原文")}
            style={{ whiteSpace: "pre-wrap", overflowWrap: "anywhere" }}
          >
            {o.body}
          </pre>
        </section>
      ) : (
        <p>{t("尚无独立重取观测；不会自动请求或补造历史证据。")}</p>
      )}
    </details>
  );
}
