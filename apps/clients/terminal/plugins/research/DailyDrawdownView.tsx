import { useState } from "react";
import type { EvaluationDailyDrawdown } from "../../src/bridge/client";
import { percent } from "./Performance";
import { t } from "./shared";

export function DailyDrawdownView({
  path,
  currency,
}: {
  path: EvaluationDailyDrawdown;
  currency: string;
}) {
  const [page, setPage] = useState(0);
  const [episodePage, setEpisodePage] = useState(0);
  const size = 50;
  const rows = path.points.slice(page * size, (page + 1) * size);
  const episodes = path.episodes.slice(episodePage * size, (episodePage + 1) * size);
  // Layout uses only saved fractions and the saved full-path scale; it never
  // infers peaks, recovers an episode or recalculates a risk statistic.
  const scale =
    path.max_fraction.value && path.max_fraction.value > 0 ? path.max_fraction.value : 1;
  const segments: string[][] = [[]];
  rows.forEach((row, i) => {
    if (row.fraction.value === null) {
      segments.push([]);
      return;
    }
    segments[segments.length - 1].push(
      `${30 + (740 * i) / Math.max(1, rows.length - 1)},${20 + (140 * row.fraction.value) / scale}`,
    );
  });
  return (
    <section aria-label={t("原日采样损失路径")}>
      <h4>{t("原日采样损失路径")}</h4>
      <p>
        {t("仅原日结算观察，不补缺日或初始资金点；本区间首个结算为峰值起点，不代表盘中最大损失。")}
      </p>
      <dl>
        <dt>{t("最大日采样回撤比例")}</dt>
        <dd>
          {percent(path.max_fraction.value)}
          {path.max_fraction.value === null &&
            ` · ${t(path.max_fraction.state === "FEWER_THAN_TWO_OBSERVATIONS" ? "不足两个日结算观察" : "非正权益使全路径比例覆盖不完整")}`}
        </dd>
        <dt>{t("最大日采样回撤金额")}</dt>
        <dd>
          {path.max_loss} {currency} · {t("原结算索引")} {path.max_loss_source_index}
        </dd>
        <dt>{t("原观察数")}</dt>
        <dd>{path.points.length}</dd>
      </dl>
      <p>
        {t(
          "曲线仅绘当前页原观察；缺比例处断开，范围外未绘制。间隔数是观察索引差，不是自然日或完整交易日数。",
        )}
      </p>
      <svg
        role="img"
        aria-label={t("当前页日采样回撤曲线")}
        viewBox="0 0 800 190"
        style={{ width: "100%", maxHeight: 220 }}
      >
        <title>{t("当前页日采样回撤曲线")}</title>
        <path d="M30 20H770 M30 160H770" stroke="currentColor" fill="none" opacity=".3" />
        <text x="0" y="17">
          0%
        </text>
        <text x="0" y="178">
          {percent(scale)}
        </text>
        {segments
          .filter(segment => segment.length)
          .map((segment, i) => (
            <polyline
              key={i}
              points={segment.join(" ")}
              stroke="currentColor"
              strokeWidth="2"
              fill="none"
            />
          ))}
      </svg>
      <p>
        {rows[0]?.trading_day} — {rows[rows.length - 1]?.trading_day}
      </p>
      <table aria-label={t("日采样原行证据")}>
        <thead>
          <tr>
            {[
              "原结算索引",
              "原交易日",
              "原UTC纳秒",
              "原权益",
              "原峰结算索引",
              "原峰权益",
              "回撤金额",
              "日采样回撤比例",
            ].map(label => (
              <th key={label}>{t(label)}</th>
            ))}
          </tr>
        </thead>
        <tbody>
          {rows.map(row => (
            <tr key={row.source_index}>
              <td>{row.source_index}</td>
              <td>{row.trading_day}</td>
              <td>{row.timestamp_ns ?? t("未记录")}</td>
              <td>
                {row.equity} {currency}
              </td>
              <td>{row.peak_source_index}</td>
              <td>
                {row.peak_equity} {currency}
              </td>
              <td>
                {row.loss} {currency}
              </td>
              <td>
                {row.fraction.value === null
                  ? t("非正权益，比例不适用")
                  : percent(row.fraction.value)}
              </td>
            </tr>
          ))}
        </tbody>
      </table>
      <button disabled={page === 0} onClick={() => setPage(page - 1)}>
        {t("上一页日观察")}
      </button>
      <button disabled={(page + 1) * size >= path.points.length} onClick={() => setPage(page + 1)}>
        {t("下一页日观察")}
      </button>
      <table aria-label={t("日采样回撤片段")}>
        <thead>
          <tr>
            {[
              "原峰结算索引",
              "原谷结算索引",
              "首次恢复结算索引",
              "峰到谷观察间隔",
              "峰到恢复观察间隔",
              "本片段已观察间隔",
            ].map(label => (
              <th key={label}>{t(label)}</th>
            ))}
          </tr>
        </thead>
        <tbody>
          {episodes.map(episode => (
            <tr key={episode.peak_source_index}>
              <td>{episode.peak_source_index}</td>
              <td>{episode.trough_source_index}</td>
              <td>{episode.recovery_source_index ?? t("尚未恢复")}</td>
              <td>{episode.peak_to_trough_intervals}</td>
              <td>{episode.peak_to_recovery_intervals ?? t("尚未恢复")}</td>
              <td>{episode.observed_intervals}</td>
            </tr>
          ))}
        </tbody>
      </table>
      {!path.episodes.length && <p>{t("本固定日观察中无回撤片段；不等于无风险。")}</p>}
      <button disabled={episodePage === 0} onClick={() => setEpisodePage(episodePage - 1)}>
        {t("上一页损失片段")}
      </button>
      <button
        disabled={(episodePage + 1) * size >= path.episodes.length}
        onClick={() => setEpisodePage(episodePage + 1)}
      >
        {t("下一页损失片段")}
      </button>
    </section>
  );
}
