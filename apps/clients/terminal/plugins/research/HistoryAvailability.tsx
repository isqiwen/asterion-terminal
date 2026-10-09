import { translate } from "../contract";
import { timestamp, type HistoryVersionEvidence } from "../../src/bridge/client";
const t = (key: string) => translate("asterion.terminal.research", key);
export function HistoryAvailability({ versions }: { versions: HistoryVersionEvidence[] }) {
  return (
    <section aria-label={t("历史可知性")}>
      <p className="subtle">
        {t("来源公布时间未知。结果用于事后统计与模型回放，不能证明历史当时可交易。")}
      </p>
      <details>
        <summary>{t("版本取得证据")}</summary>
        <p>{t("取得时间是系统首次发布完整版本的时间，不是行情发生或来源公布时间。")}</p>
        <dl className="experiment-fields">
          {versions.map(version => (
            <div key={version.dataset_id}>
              <dt>
                <code>{version.dataset_id}</code>
              </dt>
              <dd>
                {t("完整版本取得时间")}: {timestamp(version.acquired_at_ns)}
              </dd>
            </div>
          ))}
        </dl>
      </details>
    </section>
  );
}
