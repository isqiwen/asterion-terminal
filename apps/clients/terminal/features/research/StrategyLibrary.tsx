import { useState } from "react";
import { RunState, type RunSummary } from "../../src/generated/asterion/lab_pb";
import type { TradingAccounts } from "../../src/host/trading-accounts";
import { type MessageValues, translate, listSeparator } from "../../src/i18n";
import { StatusBadge, type Tone } from "../../src/ui/StatusBadge";
import { type ModuleItem, useLibrarySort } from "./LabParts";
import { metric, periodText, shortTime, signed } from "./ResearchResult";
import { strategyDeployment } from "./StrategyLifecycle";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

const stages: readonly (readonly [string, Tone])[] = [
  ["已回测", "neutral"],
  ["已部署", "neutral"],
  ["仿真", "ok"],
  ["实盘", "attention"],
];
const figures = [
  ["total_return", "总收益", true],
  ["annual_return", "年化收益", true],
  ["sharpe", "夏普比率", false],
  ["holdout_sharpe", "留出夏普", false],
  ["max_drawdown", "最大回撤", true],
] as const;
type LibraryKey = "stage" | "started" | (typeof figures)[number][0];

// Every strategy beside its latest successful backtest and how far it has gone
// toward trading. Each row shows its symbols and period: runs may differ in both.
export function StrategyLibrary({
  items,
  total,
  runs,
  views,
  chosen,
  open,
  openRun,
}: {
  items: readonly ModuleItem[];
  total: number;
  // Newest first.
  runs: readonly RunSummary[];
  views: TradingAccounts["views"];
  chosen: string;
  open: (item: ModuleItem) => void;
  openRun: (item: ModuleItem, run: string) => void;
}) {
  const { heading, sorted, status } = useLibrarySort<LibraryKey>("sharpe");
  const [groupFilter, setGroupFilter] = useState("");
  const groups = [...new Set(items.map(item => item.group!))].sort();
  // A search can leave the filtered group with no rows, and so without its option.
  const activeGroup = groups.includes(groupFilter) ? groupFilter : "";
  const rows = sorted(
    items
      .filter(item => !activeGroup || item.group === activeGroup)
      .map(item => {
        const run = runs.find(
          entry => entry.strategy === item.id && entry.state === RunState.SUCCEEDED,
        );
        const deployment = strategyDeployment(views, item.id);
        const stage = deployment.live.length
          ? 3
          : deployment.simulating.length
            ? 2
            : deployment.version
              ? 1
              : run
                ? 0
                : undefined;
        return { item, run, stage };
      }),
    (row, key) =>
      key === "stage"
        ? row.stage
        : key === "started"
          ? row.run && Number(row.run.startedMs)
          : row.run?.metrics[key],
  );
  if (!total) return <div className="lab-center" />;
  return (
    <div className="lab-center lab-library-view">
      <div className="lab-toolbar">
        <label className="lab-inline">
          <span>{t("类型")}</span>
          <select value={activeGroup} onChange={event => setGroupFilter(event.target.value)}>
            <option value="">{t("全部")}</option>
            {groups.map(group => (
              <option key={group} value={group}>
                {group}
              </option>
            ))}
          </select>
        </label>
      </div>
      {status}
      {!rows.length && total > 0 ? (
        <p className="subtle">{t("没有匹配的模块")}</p>
      ) : (
        <div
          className="lab-scroll lab-library-scroll"
          tabIndex={0}
          role="region"
          aria-label={t("策略库")}
        >
          <table className="lab-table lab-records lab-library-table" aria-label={t("策略库")}>
            <thead>
              <tr>
                <th scope="col" className="lab-name-col">
                  {t("策略")}
                </th>
                {heading("stage", t("阶段"))}
                {figures.map(([key, label]) => heading(key, t(label)))}
                <th scope="col">{t("品种")}</th>
                <th scope="col">{t("区间")}</th>
                {heading("started", t("最近回测"))}
              </tr>
            </thead>
            <tbody>
              {rows.map(({ item, run, stage }) => (
                <tr
                  key={item.id}
                  className={item.id === chosen ? "current" : undefined}
                  data-empty={run ? undefined : true}
                  title={item.description}
                  onClick={() => open(item)}
                >
                  <th scope="row">
                    <button
                      type="button"
                      className="lab-link strong"
                      onClick={event => {
                        event.stopPropagation();
                        open(item);
                      }}
                    >
                      {item.name}
                    </button>
                  </th>
                  <td className="numeric">
                    {stage === undefined ? (
                      "—"
                    ) : (
                      <StatusBadge tone={stages[stage]![1]}>{t(stages[stage]![0])}</StatusBadge>
                    )}
                  </td>
                  {figures.map(([key, label, percent]) => {
                    const value = run?.metrics[key];
                    return (
                      <td
                        key={key}
                        className={`numeric ${key.endsWith("return") ? signed(value) : ""}`}
                      >
                        {key === "total_return" && run ? (
                          <button
                            type="button"
                            className="lab-link"
                            aria-label={t("{name} {figure}", {
                              name: item.name,
                              figure: t(label),
                            })}
                            onClick={event => {
                              event.stopPropagation();
                              openRun(item, run.id);
                            }}
                          >
                            {metric(value, percent)}
                          </button>
                        ) : (
                          metric(value, percent)
                        )}
                      </td>
                    );
                  })}
                  <td className="subtle" title={run?.symbols.join(listSeparator())}>
                    {!run
                      ? "—"
                      : run.symbols.length === 1
                        ? run.symbols[0]
                        : t("{count} 个品种", { count: run.symbols.length })}
                  </td>
                  <td className="subtle">
                    {run ? `${run.firstDay} – ${run.lastDay} · ${periodText(run.period)}` : "—"}
                  </td>
                  <td className="subtle numeric">{run ? shortTime(run.startedMs) : "—"}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
    </div>
  );
}
