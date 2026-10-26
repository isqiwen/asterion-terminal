import { useMemo, useState } from "react";
import { PriceChart } from "@asterion/client-ui/PriceChart";
import { ErrorNotice, asDisplayError, type DisplayError, type TerminalContext } from "../contract";
import type { QuantStatsAnalysis } from "../../src/bridge/client";
import { namespace, t } from "./shared";

export function QuantStatsReport({
  id,
  resultDigest,
  busy,
  query,
}: {
  id: string;
  resultDigest: string;
} & Pick<TerminalContext, "busy" | "query">) {
  const [report, setReport] = useState<QuantStatsAnalysis | null>(null);
  const [pending, setPending] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  const series = useMemo(
    () => ({
      kind: "line" as const,
      points: (report?.returns ?? []).map(row => ({
        timestamp_ns: row.trading_day,
        price: String(row.value * 100),
      })),
    }),
    [report],
  );
  async function analyze() {
    setPending(true);
    setError("");
    try {
      const reply = await query("task.quantstats.analyze", { id, result_digest: resultDigest });
      if (
        !reply.quantstats_report ||
        reply.quantstats_report.task_id !== id ||
        reply.quantstats_report.result_sha256 !== resultDigest
      )
        throw new Error(t("QuantStats 返回的结果与当前回测不一致"));
      setReport(reply.quantstats_report);
    } catch (reason) {
      setError(asDisplayError(reason));
    } finally {
      setPending(false);
    }
  }
  return (
    <section aria-label={t("QuantStats 绩效分析")}>
      <h4>{t("QuantStats 绩效分析")}</h4>
      <div className="inline-actions">
        <button disabled={busy || pending} onClick={() => void analyze()}>
          {t(pending ? "正在分析…" : "生成 QuantStats 报告")}
        </button>
        <details className="subtle">
          <summary>{t("说明")}</summary>
          <p>
            {t(
              "按全程实际结算权益计算扣费后日收益，包含训练段；不补齐缺失交易日。年化按 252 日，无风险利率为 0。至少需要 20 个结算日且权益始终为正。",
            )}
          </p>
        </details>
      </div>
      <ErrorNotice error={error} namespace={namespace} />
      {report && (
        <>
          <p className="subtle">
            QuantStats {report.library_version} · Polars {report.polars_version} · {t("交易日数")}:{" "}
            {report.returns.length}
          </p>
          <div className="research-metrics">
            {(
              [
                ["sortino", "索提诺比率", false],
                ["omega", "Omega 比率", false],
                ["tail_ratio", "尾部比率", false],
                ["ulcer_index", "溃疡指数", true],
                ["value_at_risk", "日 VaR（95%）", true],
                ["conditional_value_at_risk", "日 CVaR（95%）", true],
              ] as const
            ).map(([key, label, percent]) => (
              <div key={key}>
                <span className="subtle">{t(label)}</span>
                <strong>
                  {report.metrics[key] === null
                    ? "—"
                    : percent
                      ? `${(report.metrics[key] * 100).toFixed(3)}%`
                      : report.metrics[key].toFixed(3)}
                </strong>
              </div>
            ))}
          </div>
          <p className="subtle">
            {t(
              "VaR 与 CVaR 均使用收益正态分布估计，CVaR 为最差 5% 情形的条件期望；负值表示损失。无定义指标显示为 —。这些统计不参与交易风控或策略选择。",
            )}
          </p>
          <PriceChart
            series={series}
            label={t("日收益率")}
            volumeLabel={t("成交量")}
            showVolume={false}
            compact
            formatTime={value => value}
            formatPrice={value => `${value.toFixed(2)}%`}
            describe={index =>
              `${report.returns[index].trading_day} · ${(report.returns[index].value * 100).toFixed(3)}%`
            }
          />
          <details>
            <summary>{t("分析来源")}</summary>
            <p>{id}</p>
            <code>{report.result_sha256}</code>
          </details>
        </>
      )}
    </section>
  );
}
