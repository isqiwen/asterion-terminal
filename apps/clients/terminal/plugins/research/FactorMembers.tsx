import { timestamp, type FactorExperiment, type FactorResult } from "../../src/bridge/client";
import { factorLabel, t } from "./shared";

export function FactorMembers({
  experiment,
  section,
  lookback,
}: {
  experiment: Pick<FactorExperiment, "series" | "factor">;
  section: FactorResult["cross_sections"][number];
  lookback: number;
}) {
  const when = (value: string) =>
    experiment.series[0].kind === "table" ? value : timestamp(value);
  const percent = (value: number) => `${(value * 100).toFixed(4)}%`;
  return (
    <section aria-label={t("截面成员证据")}>
      <h4>{t("截面成员证据")}</h4>
      <p>
        {when(section.observed)} → {when(section.label)} · {t("实际窗口")}: {lookback} ·{" "}
        {t("实际评价成员数")}: {section.members.length}
      </p>
      <p className="subtle">
        {t(
          "按因子值从高到低列出实际参与成员，并列按输入顺序；这是分组顺序，秩 IC 仍使用平均秩。未列出者未参与此观测，资格通过不代表特征和标签完整。未来收益是评价标签，不是扣费交易收益。",
        )}
      </p>
      {section.members.length === 0 ? (
        <p>{t("此观测没有实际参与成员")}</p>
      ) : (
        <div className="research-table">
          <table aria-label={t("截面成员明细")}>
            <thead>
              <tr>
                <th>{t("分组顺序")}</th>
                <th>{t("品种或合约")}</th>
                <th>{factorLabel(experiment.factor)}</th>
                <th>{t("未来收益")}</th>
              </tr>
            </thead>
            <tbody>
              {section.members.map((member, index) => {
                const series = experiment.series[member.series_index];
                const contract =
                  series.kind === "bars"
                    ? series.dataset.contract
                    : series.kind === "dominant"
                      ? series.months[0].dataset.contract
                      : null;
                const instrument =
                  series.kind === "table"
                    ? series.data.instrument
                    : `${contract!.venue}/${series.kind === "dominant" ? contract!.product : contract!.symbol}`;
                const continuous =
                  series.kind === "dominant" ||
                  (series.kind === "table" && series.data.rolls.length > 0);
                return (
                  <tr key={member.series_index}>
                    <td>{index + 1}</td>
                    <td>
                      {instrument}
                      {continuous && ` · ${t("主力连续")}`}
                    </td>
                    <td>{percent(member.value)}</td>
                    <td>{percent(member.forward_return)}</td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
      )}
    </section>
  );
}
