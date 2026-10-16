import { useMemo, useState } from "react";
import { PriceChart } from "@asterion/client-ui/PriceChart";
import type { PortfolioAnalysis } from "../../src/bridge/client";
import { ErrorNotice, asDisplayError, type DisplayError, type TerminalContext } from "../contract";
import { namespace, t } from "./shared";

const percent = (value: number) => `${(value * 100).toFixed(4)}%`;

export function PortfolioReport({
  sources,
  busy,
  query,
}: { sources: { id: string; result_digest: string }[] } & Pick<TerminalContext, "busy" | "query">) {
  const [trainingEnd, setTrainingEnd] = useState("");
  const [report, setReport] = useState<PortfolioAnalysis | null>(null);
  const [pending, setPending] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  const series = useMemo(
    () => ({
      kind: "line" as const,
      points: (report?.validation ?? []).map(row => ({
        timestamp_ns: row.trading_day,
        price: String(row.optimized_return * 100),
      })),
    }),
    [report],
  );
  async function analyze() {
    setPending(true);
    setReport(null);
    setError("");
    try {
      const reply = await query("task.portfolio.analyze", {
        expected: sources,
        training_end: trainingEnd,
      });
      const value = reply.portfolio_report;
      if (
        !value ||
        value.training_end !== trainingEnd ||
        value.weights.length !== sources.length ||
        value.weights.some(
          (weight, i) =>
            weight.task_id !== sources[i].id || weight.result_sha256 !== sources[i].result_digest,
        )
      )
        throw new Error(t("组合分析返回的结果与当前实验或训练边界不一致"));
      setReport(value);
    } catch (reason) {
      setError(asDisplayError(reason));
    } finally {
      setPending(false);
    }
  }
  return (
    <section aria-label={t("回测组合研究")}>
      <h4>{t("回测组合研究")}</h4>
      <p className="subtle">
        {t(
          "Polars 按相同结算收益区间对齐，不补零。训练截止日及以前拟合，之后验证，两段各需至少 20 个共同观测。skfolio 使用 Ledoit–Wolf 协方差与 CLARABEL 求解非负、合计为一的最小方差配比。",
        )}
      </p>
      <p className="subtle">
        {t(
          "配比仅组合独立回测的收益流，不代表期货手数或可执行组合。原回测费用已包含，未计组合再平衡费用。原策略已回看或选优的数据不因此成为新的样本外证据。",
        )}
      </p>
      <fieldset disabled={busy || pending}>
        <label>
          {t("组合训练截止日（含）")}
          <input
            type="date"
            value={trainingEnd}
            onChange={event => {
              setTrainingEnd(event.target.value);
              setReport(null);
              setError("");
            }}
          />
        </label>
        <button disabled={!trainingEnd} onClick={() => void analyze()}>
          {t(pending ? "正在分析…" : "分析回测组合")}
        </button>
      </fieldset>
      <ErrorNotice error={error} namespace={namespace} />
      {report && (
        <>
          <p className="subtle">
            Polars {report.polars_version} · skfolio {report.skfolio_version}
          </p>
          <p>
            {t("实际训练区间")}: {report.training_begin} – {report.training_last} ·{" "}
            {report.training_observations} {t("个共同观测")}
          </p>
          <p>
            {t("实际验证区间")}: {report.validation[0].trading_day} –{" "}
            {report.validation.at(-1)!.trading_day} · {report.validation.length} {t("个共同观测")}
          </p>
          <div className="table-scroll">
            <table aria-label={t("组合配比与来源")}>
              <thead>
                <tr>
                  <th>{t("任务 ID")}</th>
                  <th>{t("结果摘要")}</th>
                  <th>{t("最小方差配比")}</th>
                  <th>{t("等权配比")}</th>
                  <th>{t("原始观测数")}</th>
                  <th>{t("未对齐排除数")}</th>
                </tr>
              </thead>
              <tbody>
                {report.weights.map(weight => (
                  <tr key={weight.task_id}>
                    <td>{weight.task_id}</td>
                    <td>
                      <code>{weight.result_sha256}</code>
                    </td>
                    <td>{percent(weight.weight)}</td>
                    <td>{percent(1 / report.weights.length)}</td>
                    <td>{weight.source_observations}</td>
                    <td>
                      {weight.source_observations -
                        report.training_observations -
                        report.validation.length}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
          <table aria-label={t("组合验证绩效")}>
            <thead>
              <tr>
                <th>{t("指标")}</th>
                <th>{t("最小方差配比")}</th>
                <th>{t("等权配比")}</th>
              </tr>
            </thead>
            <tbody>
              <tr>
                <th scope="row">{t("验证总收益率")}</th>
                <td>{percent(report.optimized_total_return)}</td>
                <td>{percent(report.equal_weight_total_return)}</td>
              </tr>
              <tr>
                <th scope="row">{t("验证年化波动率")}</th>
                <td>{percent(report.optimized_annual_volatility)}</td>
                <td>{percent(report.equal_weight_annual_volatility)}</td>
              </tr>
            </tbody>
          </table>
          <p className="subtle">
            {t(
              "验证收益按固定权重逐期配比并复利累计；年化波动率按每年 252 个观测、样本标准差计算。未对齐区间已排除，图表仅显示实际共同观测。",
            )}
          </p>
          <PriceChart
            series={series}
            label={t("组合验证收益率")}
            volumeLabel={t("成交量")}
            showVolume={false}
            compact
            overlays={[
              {
                label: t("等权配比"),
                color: "#e0b66b",
                values: report.validation.map(row => row.equal_weight_return * 100),
              },
            ]}
            formatTime={value => value}
            formatPrice={value => `${value.toFixed(2)}%`}
            formatIndicatorValue={value => `${value.toFixed(4)}%`}
            describe={index =>
              `${report.validation[index].trading_day} · ${t("最小方差配比")}: ${percent(report.validation[index].optimized_return)} · ${t("等权配比")}: ${percent(report.validation[index].equal_weight_return)}`
            }
          />
        </>
      )}
    </section>
  );
}
