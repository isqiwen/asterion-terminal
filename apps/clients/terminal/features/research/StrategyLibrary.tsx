import { useEffect, useRef, useState } from "react";
import { lab } from "../../src/bridge/lab";
import { RunState, type RunSummary } from "../../src/generated/asterion/lab_pb";
import { useSequentialPoll } from "../../src/host/polling";
import type { TradingAccounts } from "../../src/host/trading-accounts";
import { type MessageValues, translate, listSeparator } from "../../src/i18n";
import { StatusBadge, type Tone } from "../../src/ui/StatusBadge";
import { type ModuleItem, useLibrarySort } from "./LabParts";
import { metric, periodText, runTones, shortTime, signed, states } from "./ResearchResult";
import { batchStrategy, runState } from "./research-lab";
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
export type LibraryScope = "all" | "batch";
export type StrategyBatch = { id: string; runs: readonly RunSummary[]; active: boolean };

// A batch's runs, read until the last one ends. One deleted from the backtest
// records reads as none, and the library shows all strategies again.
export function useStrategyBatch(id: string, ended: () => void) {
  const [batch, setBatch] = useState<StrategyBatch | null>(null);
  useSequentialPoll(
    async isActive => {
      try {
        const reply = await lab({ case: "getSweep", value: { id } });
        if (!isActive()) return false;
        setBatch({ id, runs: reply.runs, active: reply.active });
        if (!reply.active) ended();
        return reply.active;
      } catch {
        if (isActive()) setBatch(null);
        return false;
      }
    },
    1500,
    Boolean(id),
    [id, ended],
  );
  return batch?.id === id ? batch : null;
}

// Every strategy beside its latest successful backtest and how far it has gone
// toward trading; or one batch's strategies, all on the same settings.
export function StrategyLibrary({
  items,
  total,
  runs,
  views,
  chosen,
  selected,
  setSelected,
  batch,
  scope,
  setScope,
  cancelBatch,
  open,
  openRun,
}: {
  items: readonly ModuleItem[];
  total: number;
  // Newest first.
  runs: readonly RunSummary[];
  views: TradingAccounts["views"];
  chosen: string;
  selected: readonly string[];
  setSelected: (next: string[]) => void;
  batch: StrategyBatch | null;
  scope: LibraryScope;
  setScope: (scope: LibraryScope) => void;
  cancelBatch: () => void;
  open: (item: ModuleItem) => void;
  openRun: (item: ModuleItem, run: string) => void;
}) {
  const { heading, sorted, status } = useLibrarySort<LibraryKey>("sharpe");
  const [groupFilter, setGroupFilter] = useState("");
  const selectAllRef = useRef<HTMLInputElement>(null);
  const groups = [...new Set(items.map(item => item.group!))].sort();
  // A search can leave the filtered group with no rows, and so without its option.
  const activeGroup = groups.includes(groupFilter) ? groupFilter : "";
  const shownBatch = scope === "batch" ? batch : null;
  const memberOf = (id: string) => shownBatch?.runs.find(run => batchStrategy(run) === id);
  const rows = sorted(
    items
      .filter(item => !activeGroup || item.group === activeGroup)
      .filter(item => !shownBatch || memberOf(item.id))
      .map(item => {
        const latest = shownBatch
          ? memberOf(item.id)
          : runs.find(entry => entry.strategy === item.id);
        const run =
          latest?.state === RunState.SUCCEEDED
            ? latest
            : shownBatch
              ? undefined
              : runs.find(
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
        return { item, run, latest, stage };
      }),
    (row, key) =>
      key === "stage"
        ? row.stage
        : key === "started"
          ? row.run && Number(row.run.startedMs)
          : row.run?.metrics[key],
  );
  const shownIds = rows.map(row => row.item.id);
  const selectedShown = shownIds.filter(id => selected.includes(id));
  const allShown = shownIds.length > 0 && selectedShown.length === shownIds.length;
  const someShown = selectedShown.length > 0 && !allShown;
  useEffect(() => {
    if (selectAllRef.current) selectAllRef.current.indeterminate = someShown;
  }, [someShown]);
  // The settings panel takes the width of what it sets.
  const compact = selected.length > 0;
  const ended = batch?.runs.filter(
    run => run.state !== RunState.UNSPECIFIED && run.state !== RunState.RUNNING,
  ).length;
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
        {batch && (
          <label className="lab-inline">
            <span>{t("范围")}</span>
            <select value={scope} onChange={event => setScope(event.target.value as LibraryScope)}>
              <option value="all">{t("全部策略")}</option>
              <option value="batch">{t("本次批量")}</option>
            </select>
          </label>
        )}
        {batch?.active && (
          <>
            <span className="subtle numeric">
              {t("已完成 {done} / {count}", { done: ended ?? 0, count: batch.runs.length })}
            </span>
            <button type="button" className="lab-link" onClick={cancelBatch}>
              {t("取消")}
            </button>
          </>
        )}
        <span className="lab-spacer" />
        {selected.length > 0 && (
          <button type="button" className="lab-link" onClick={() => setSelected([])}>
            {t("清空选择")}
          </button>
        )}
      </div>
      {status}
      {!rows.length ? (
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
                <th scope="col" className="lab-check-col">
                  <input
                    ref={selectAllRef}
                    type="checkbox"
                    aria-label={t("全选策略")}
                    checked={allShown}
                    onChange={event =>
                      setSelected(
                        event.target.checked
                          ? [...new Set([...selected, ...shownIds])]
                          : selected.filter(id => !shownIds.includes(id)),
                      )
                    }
                  />
                </th>
                <th scope="col" className="lab-name-col">
                  {t("策略")}
                </th>
                {heading("stage", t("阶段"))}
                {figures.map(([key, label]) => heading(key, t(label)))}
                {!compact && <th scope="col">{t("品种")}</th>}
                {!compact && <th scope="col">{t("区间")}</th>}
                {heading("started", t("最近回测"))}
              </tr>
            </thead>
            <tbody>
              {rows.map(({ item, run, latest, stage }) => {
                const checked = selected.includes(item.id);
                const unfinished = latest && latest.state !== RunState.SUCCEEDED;
                const idle = run?.metrics.fills === 0;
                return (
                  <tr
                    key={item.id}
                    className={checked ? "selected" : item.id === chosen ? "current" : undefined}
                    data-empty={run && !idle ? undefined : true}
                    title={item.description}
                    onClick={() => open(item)}
                  >
                    <td className="lab-check-col" onClick={event => event.stopPropagation()}>
                      <input
                        type="checkbox"
                        aria-label={t("选择 {name}", { name: item.id })}
                        checked={checked}
                        onChange={event =>
                          setSelected(
                            event.target.checked
                              ? [...selected, item.id]
                              : selected.filter(id => id !== item.id),
                          )
                        }
                      />
                    </td>
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
                    {!compact && (
                      <td className="subtle" title={run?.symbols.join(listSeparator())}>
                        {!run
                          ? "—"
                          : run.symbols.length === 1
                            ? run.symbols[0]
                            : t("{count} 个品种", { count: run.symbols.length })}
                      </td>
                    )}
                    {!compact && (
                      <td className="subtle">
                        {run ? `${run.firstDay} – ${run.lastDay} · ${periodText(run.period)}` : "—"}
                      </td>
                    )}
                    <td className="subtle numeric" title={unfinished ? latest.error : undefined}>
                      {unfinished ? (
                        <StatusBadge tone={runTones[runState(latest.state)] ?? "neutral"}>
                          {t(states[runState(latest.state)] ?? "未开始")}
                        </StatusBadge>
                      ) : idle ? (
                        <StatusBadge tone="neutral">{t("无成交")}</StatusBadge>
                      ) : run ? (
                        shortTime(run.startedMs)
                      ) : (
                        "—"
                      )}
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
      )}
    </div>
  );
}
