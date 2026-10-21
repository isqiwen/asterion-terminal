import type { NodeStatus } from "../bridge/client";
import { translate, useLocale } from "../i18n";
import { BackendError, ErrorNotice } from "../i18n/errors";

export function NodeResources({ node }: { node: NodeStatus }) {
  const { locale } = useLocale();
  const t = (key: string, values = {}) => translate("host", key, values);
  const sample = node.health?.host_resources;
  if (!sample) return <p className="subtle">{t("主机资源尚无观测")}</p>;
  return (
    <section aria-label={t("主机资源观测")}>
      <p className="subtle">
        {t(
          node.state === "online" ? "主机资源观测于 {time}" : "节点离线，主机资源上次观测于 {time}",
          {
            time: new Date(sample.sampled_at_ms).toLocaleString(locale),
          },
        )}
      </p>
      {sample.failure ? (
        <ErrorNotice error={new BackendError(sample.failure.code, sample.failure.message)} />
      ) : (
        <p className="subtle">
          {t("主机 CPU：{cpu} · 可用内存（估计）：{memory} GiB · 节点目录磁盘可用：{disk} GiB", {
            cpu:
              sample.cpu_usage_percent === null
                ? t("等待 CPU 采样区间")
                : `${sample.cpu_usage_percent.toFixed(1)}%`,
            memory: (sample.memory_available_bytes / 1024 ** 3).toFixed(1),
            disk: (sample.disk_available_bytes / 1024 ** 3).toFixed(1),
          })}
        </p>
      )}
      <p className="subtle">{t("磁盘观测目录：{path}", { path: sample.disk_path })}</p>
    </section>
  );
}
