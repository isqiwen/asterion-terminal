import { useEffect, useId, useRef, useState, type ReactNode } from "react";
import { lab } from "../../src/bridge/lab";
import { RunState, type RunSummary } from "../../src/generated/asterion/lab_pb";
import { Environment, InstanceState } from "../../src/generated/asterion/trading_pb";
import { useSequentialPoll } from "../../src/host/polling";
import type { TradingAccounts } from "../../src/host/trading-accounts";
import { type MessageValues, translate } from "../../src/i18n";
import { StatusBadge, type Tone } from "../../src/ui/StatusBadge";
import { filterModuleItems, type ModuleItem, useLibrarySort } from "./LabParts";
import { metric, runTones, shortTime, signed, states } from "./ResearchResult";
import { batchStrategy, runState } from "./research-lab";

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
  ["sharpe", "夏普比率", false],
  ["holdout_sharpe", "留出夏普", false],
  ["max_drawdown", "最大回撤", true],
] as const;
type LibraryKey = "stage" | "started" | (typeof figures)[number][0];
const running = new Set([InstanceState.STARTING, InstanceState.WARMING, InstanceState.READY]);

// How far a strategy has gone toward trading, as an index into `stages`.
function stageOf(views: TradingAccounts["views"], strategy: string, tested: boolean) {
  const accounts = Object.values(views);
  const hosts = accounts.filter(view => {
    const instance = view.strategies?.instance;
    return instance?.instance?.packageName === strategy && running.has(instance.state);
  });
  if (hosts.some(view => view.account?.environment === Environment.REAL)) return 3;
  if (hosts.length) return 2;
  if (accounts.some(view => view.strategies?.packages.some(item => item.name === strategy)))
    return 1;
  return tested ? 0 : undefined;
}

export type StrategyBatch = { id: string; runs: readonly RunSummary[]; active: boolean };

// A batch's runs, read until the last one ends.
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

// The run a strategy ranks by: its latest success on the current settings, or
// else its latest success. Runs are newest first.
export function rankedRun(
  runs: readonly RunSummary[],
  strategy: string,
  onScope?: (run: RunSummary) => boolean,
) {
  const own = runs.filter(run => run.strategy === strategy && run.state === RunState.SUCCEEDED);
  return own.find(run => !onScope || onScope(run)) ?? own[0];
}

// Every strategy beside its result on the current settings. Results on other
// settings and strategies without one are listed apart, so they never rank.
export function StrategyLibrary({
  items,
  runs,
  views,
  chosen,
  selected,
  setSelected,
  batch,
  onScope,
  search,
  setSearch,
  busy,
  startBatch,
  cancelBatch,
  open,
  contextTarget,
  onMenu,
  newModule,
  notices,
}: {
  items: readonly ModuleItem[];
  // Newest first.
  runs: readonly RunSummary[];
  views: TradingAccounts["views"];
  chosen: string;
  selected: readonly string[];
  setSelected: (next: string[]) => void;
  batch: StrategyBatch | null;
  // Absent until the settings name a symbol.
  onScope?: (run: RunSummary) => boolean;
  search: string;
  setSearch: (search: string) => void;
  busy: boolean;
  startBatch: () => void;
  cancelBatch: () => void;
  open: (item: ModuleItem, run?: string) => void;
  contextTarget?: string;
  onMenu: (path: string, x: number, y: number, symbol?: string, target?: string) => void;
  newModule: ReactNode;
  notices: ReactNode;
}) {
  const { heading, sorted, status } = useLibrarySort<LibraryKey>("sharpe");
  const [groupFilter, setGroupFilter] = useState("");
  const filterName = useId();
  const selectAllRef = useRef<HTMLInputElement>(null);
  const groups = [...new Set(items.map(item => item.group!))].sort();
  // Deleting a group's last strategy removes its option.
  const activeGroup = groups.includes(groupFilter) ? groupFilter : "";
  const members = batch?.active ? batch.runs : [];
  const rows = sorted(
    filterModuleItems(items, search)
      .filter(item => !activeGroup || item.group === activeGroup)
      .map(item => {
        const member = members.find(run => batchStrategy(run) === item.id);
        const own = runs.filter(entry => entry.strategy === item.id);
        const run = rankedRun(member ? [member, ...own] : own, item.id, onScope);
        const latest = member ?? own.find(entry => !onScope || onScope(entry)) ?? own[0];
        const place = !run ? 2 : !onScope || onScope(run) ? 0 : 1;
        return { item, run, latest, place, stage: stageOf(views, item.id, Boolean(run)) };
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
  const ended = batch?.runs.filter(
    run => run.state !== RunState.UNSPECIFIED && run.state !== RunState.RUNNING,
  ).length;
  const row = ({ item, run, latest, stage }: (typeof rows)[number]) => {
    const checked = selected.includes(item.id);
    const unfinished = latest && latest.state !== RunState.SUCCEEDED;
    const idle = run?.metrics.fills === 0;
    return (
      <tr
        key={item.id}
        className={checked ? "selected" : item.id === chosen ? "current" : undefined}
        data-empty={run && !idle ? undefined : true}
        data-context={contextTarget === item.id || undefined}
        title={item.description}
        onClick={() => open(item, run?.id)}
        onContextMenu={event => {
          event.preventDefault();
          onMenu(`strategies/${item.module}.py`, event.clientX, event.clientY, item.name, item.id);
        }}
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
              open(item, run?.id);
            }}
          >
            {item.name}
          </button>
        </th>
        <td className="numeric">
          {stage !== undefined && (
            <StatusBadge tone={stages[stage]![1]}>{t(stages[stage]![0])}</StatusBadge>
          )}
        </td>
        {figures.map(([key, , percent]) => {
          const value = run?.metrics[key];
          return (
            <td key={key} className={`numeric ${key.endsWith("return") ? signed(value) : ""}`}>
              {run && metric(value, percent)}
            </td>
          );
        })}
        <td className="subtle numeric" title={unfinished ? latest.error : undefined}>
          {unfinished ? (
            <StatusBadge tone={runTones[runState(latest.state)] ?? "neutral"}>
              {t(states[runState(latest.state)] ?? "未开始")}
            </StatusBadge>
          ) : idle ? (
            <StatusBadge tone="neutral">{t("无成交")}</StatusBadge>
          ) : (
            run && shortTime(run.startedMs)
          )}
        </td>
      </tr>
    );
  };
  return (
    <div className="lab-center lab-library-view">
      <div className="lab-toolbar">
        <input
          type="search"
          aria-label={t("搜索策略")}
          placeholder={t("搜索策略")}
          spellCheck={false}
          autoCorrect="off"
          autoCapitalize="none"
          value={search}
          onChange={event => setSearch(event.target.value)}
        />
        <fieldset className="lab-segmented">
          <legend className="sr-only">{t("类型")}</legend>
          {["", ...groups].map(group => (
            <label key={group}>
              <input
                type="radio"
                name={filterName}
                checked={activeGroup === group}
                onChange={() => setGroupFilter(group)}
              />
              {group || t("全部")}
            </label>
          ))}
        </fieldset>
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
        {newModule}
        <button
          type="button"
          className="primary"
          disabled={busy || selected.length < 2}
          onClick={startBatch}
        >
          {selected.length ? t("批量回测（{count}）", { count: selected.length }) : t("批量回测")}
        </button>
      </div>
      {notices}
      {status}
      {rows.length > 0 ? (
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
                {heading("started", t("最近回测"))}
              </tr>
            </thead>
            {([0, 1, 2] as const).map(place => {
              const group = rows.filter(entry => entry.place === place);
              if (!group.length) return null;
              return (
                <tbody key={place}>
                  {place > 0 && (
                    <tr className="lab-library-group">
                      <th scope="colgroup" colSpan={figures.length + 4}>
                        {t(place === 1 ? "口径不同" : "未回测")}
                      </th>
                    </tr>
                  )}
                  {group.map(row)}
                </tbody>
              );
            })}
          </table>
        </div>
      ) : (
        items.length > 0 && <p className="subtle">{t("没有匹配的模块")}</p>
      )}
    </div>
  );
}
