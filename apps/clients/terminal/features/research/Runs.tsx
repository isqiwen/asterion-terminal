import { useCallback, useEffect, useMemo, useState } from "react";
import { lab } from "../../src/bridge/lab";
import { Period } from "../../src/generated/asterion/common_pb";
import type { RunSummary } from "../../src/generated/asterion/lab_pb";
import { ActionError, useActionError } from "../../src/host/action-error";
import type { TerminalContext } from "../../src/host/feature-registry";
import { notify } from "../../src/host/notifications";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { type MessageValues, translate, listSeparator } from "../../src/i18n";
import { StatusBadge } from "../../src/ui/StatusBadge";
import { DeleteRecord } from "./LabParts";
import {
  RunComparison,
  RunResult,
  SweepResult,
  metric,
  runTones,
  signed,
  states,
} from "./ResearchResult";
import { parameterText, runState } from "./research-lab";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

// At most this many runs compare at once: more lines no longer read apart.
const compareAtMost = 6;
const columns = [
  ["total_return", "总收益", true],
  ["annual_return", "年化收益", true],
  ["sharpe", "夏普比率", false],
  ["max_drawdown", "最大回撤", true],
  ["calmar", "卡玛比率", false],
] as const;
type SortKey = "started" | "fills" | (typeof columns)[number][0];
type Shown = { kind: "run" | "sweep"; id: string } | { kind: "compare"; ids: string[] } | null;

const periodText = (period: Period) =>
  period === Period.DAY
    ? t("日线")
    : t("{count} 分钟", { count: Period[period]?.replace("MINUTES_", "") ?? "?" });
const paramsText = (run: RunSummary) =>
  Object.entries(run.params)
    .map(([name, value]) => `${name}=${parameterText(value)}`)
    .join(" ");
export const runName = (run: RunSummary) => run.label || run.id;

// When a run started, as month-day and time of the viewer's clock.
function started(ms: bigint) {
  const time = new Date(Number(ms));
  const two = (value: number) => String(value).padStart(2, "0");
  return `${two(time.getMonth() + 1)}-${two(time.getDate())} ${two(time.getHours())}:${two(time.getMinutes())}`;
}

// Every run and sweep so far: a table to find, rank and compare them, and
// each one's report. The strategy view opens a new one: param `run` or `sweep`.
export function Runs(context: TerminalContext) {
  const [runs, setRuns] = useState<RunSummary[] | null>(null);
  const [shown, setShown] = useState<Shown>(null);
  const [sweep, setSweep] = useState<{
    id: string;
    runs: RunSummary[];
    selected: string;
    active: boolean;
  } | null>(null);
  const [compared, setCompared] = useState<string[]>([]);
  const [strategy, setStrategy] = useWorkspaceDraft("runs-strategy", "");
  const [search, setSearch] = useWorkspaceDraft("runs-search", "");
  const [sort, setSort] = useState<{ key: SortKey; descending: boolean }>({
    key: "started",
    descending: true,
  });
  const acted = useActionError();
  const opened = context.workspaceParams;
  useEffect(() => {
    if (opened?.run) setShown({ kind: "run", id: opened.run });
    else if (opened?.sweep) setShown({ kind: "sweep", id: opened.sweep });
  }, [opened?.run, opened?.sweep]);

  const readRuns = useCallback(async () => {
    try {
      setRuns((await lab({ case: "listRuns", value: {} })).runs);
    } catch (failure) {
      notify(failure);
    }
  }, []);
  useEffect(() => void readRuns(), [readRuns]);
  // Read again while something runs, or the sweep on show has runs to come.
  const running =
    (runs ?? []).some(run => runState(run.state) === "running") ||
    (shown?.kind === "sweep" && sweep?.id === shown.id && sweep.active);
  useEffect(() => {
    if (!running) return;
    const timer = setInterval(() => void readRuns(), 2000);
    return () => clearInterval(timer);
  }, [running, readRuns]);
  useEffect(() => {
    if (shown?.kind !== "sweep") return;
    let active = true;
    void lab({ case: "getSweep", value: { id: shown.id } })
      .then(reply => {
        if (!active) return;
        setSweep({
          id: shown.id,
          runs: [...reply.runs],
          selected: reply.selectedId,
          active: reply.active,
        });
      })
      .catch(failure => active && notify(failure));
    return () => {
      active = false;
    };
  }, [shown, runs]);

  // A sweep's runs show as one row.
  const rows = useMemo(() => {
    const seen = new Set<string>();
    const query = search.trim().toUpperCase();
    const value = (run: RunSummary) =>
      sort.key === "started" ? Number(run.startedMs) : (run.metrics[sort.key] ?? null);
    return (runs ?? [])
      .filter(run => {
        if (run.sweepId && (seen.has(run.sweepId) || !seen.add(run.sweepId))) return false;
        if (strategy && run.strategy !== strategy) return false;
        return (
          !query ||
          [runName(run), run.id, run.sweepId, ...run.symbols]
            .join(" ")
            .toUpperCase()
            .includes(query)
        );
      })
      .sort((a, b) => {
        const [x, y] = [value(a), value(b)];
        // Runs without the figure go last either way.
        if (x === null || y === null) return x === null ? (y === null ? 0 : 1) : -1;
        return sort.descending ? y - x : x - y;
      });
  }, [runs, strategy, search, sort]);
  const strategies = [...new Set((runs ?? []).map(run => run.strategy))].sort();
  const byId = new Map((runs ?? []).map(run => [run.id, run]));
  const back = () => setShown(null);
  const remove = (id: string) =>
    void acted.attempt(async () => {
      // A run of a sweep takes the whole sweep.
      await lab({ case: "deleteRun", value: { id } });
      setCompared(previous => previous.filter(item => item !== id));
      setShown(null);
      await readRuns();
    });
  const cancel = (run: RunSummary) =>
    void acted.attempt(async () => {
      await (run.sweepId
        ? lab({ case: "cancelSweep", value: { id: run.sweepId } })
        : lab({ case: "cancelRun", value: { id: run.id } }));
      await readRuns();
    });

  if (shown?.kind === "run") {
    const run = byId.get(shown.id);
    return (
      <section className="lab-runs-view">
        <RunHeader
          run={run}
          id={shown.id}
          back={back}
          renamed={readRuns}
          cancel={run && (() => cancel(run))}
          remove={() => remove(shown.id)}
        />
        <ActionError error={acted.error} />
        <RunResult
          key={shown.id}
          id={shown.id}
          state={runState(run?.state ?? 0)}
          openVersion={version =>
            context.navigate("workspace.data-coverage", { params: { version } })
          }
        />
      </section>
    );
  }
  if (shown?.kind === "sweep")
    return (
      <section className="lab-runs-view">
        <header className="lab-report-header">
          <button className="lab-link" onClick={back}>
            {t("← 回测记录")}
          </button>
          <h3>{t("扫描 {id}", { id: shown.id })}</h3>
        </header>
        {sweep?.id === shown.id ? (
          <SweepResult
            runs={sweep.runs}
            selected={sweep.selected}
            open={id => setShown({ kind: "run", id })}
            rolling={
              byId.has(`${shown.id}-rolling`)
                ? () => setShown({ kind: "run", id: `${shown.id}-rolling` })
                : undefined
            }
          />
        ) : (
          <p className="subtle">{t("正在读取结果…")}</p>
        )}
      </section>
    );
  if (shown?.kind === "compare")
    return (
      <section className="lab-runs-view">
        <header className="lab-report-header">
          <button className="lab-link" onClick={back}>
            {t("← 回测记录")}
          </button>
          <h3>{t("比较 {count} 次运行", { count: shown.ids.length })}</h3>
        </header>
        <RunComparison ids={shown.ids} />
      </section>
    );

  const sortBy = (key: SortKey) =>
    setSort(previous => ({
      key,
      descending:
        previous.key === key ? !previous.descending : !/drawdown|volatility|ulcer/.test(key),
    }));
  const heading = (key: SortKey, label: string) => (
    <th
      className="numeric"
      aria-sort={sort.key === key ? (sort.descending ? "descending" : "ascending") : undefined}
    >
      <button className="lab-sort" onClick={() => sortBy(key)}>
        {t(label)}
        {sort.key === key ? (sort.descending ? " ↓" : " ↑") : ""}
      </button>
    </th>
  );
  return (
    <section className="lab-runs-view">
      <div className="lab-toolbar">
        <select
          aria-label={t("策略")}
          value={strategy}
          onChange={event => setStrategy(event.target.value)}
        >
          <option value="">{t("全部策略")}</option>
          {strategies.map(item => (
            <option key={item}>{item}</option>
          ))}
        </select>
        <input
          type="search"
          aria-label={t("搜索名称或品种")}
          placeholder={t("搜索名称或品种")}
          value={search}
          onChange={event => setSearch(event.target.value)}
        />
        <span className="lab-spacer" />
        <button
          className="primary"
          disabled={compared.length < 2}
          onClick={() => setShown({ kind: "compare", ids: compared })}
        >
          {t("比较所选（{count}）", { count: compared.length })}
        </button>
      </div>
      <ActionError error={acted.error} />
      {!runs ? (
        <p className="subtle" role="status">
          {t("读取中")}
        </p>
      ) : !rows.length ? (
        <p className="lab-placeholder">{t(runs.length ? "没有匹配的运行" : "还没有运行")}</p>
      ) : (
        <div className="lab-scroll">
          <table className="lab-table lab-records" aria-label={t("回测记录")}>
            <thead>
              <tr>
                <th />
                <th>{t("名称")}</th>
                <th>{t("品种")}</th>
                <th>{t("区间")}</th>
                <th>{t("参数")}</th>
                {columns.map(([key, label]) => heading(key, label))}
                {heading("fills", "成交笔数")}
                <th>{t("状态")}</th>
                {heading("started", "开始时间")}
                <th />
              </tr>
            </thead>
            <tbody>
              {rows.map(run => {
                const group = run.sweepId;
                const members = group ? (runs ?? []).filter(item => item.sweepId === group) : [];
                const active = group
                  ? members.some(item => runState(item.state) === "running")
                  : runState(run.state) === "running";
                const comparable = !group && runState(run.state) === "succeeded";
                return (
                  <tr key={group || run.id}>
                    <td>
                      {!group && (
                        <input
                          type="checkbox"
                          aria-label={t("比较 {id}", { id: runName(run) })}
                          checked={compared.includes(run.id)}
                          disabled={
                            !comparable ||
                            (!compared.includes(run.id) && compared.length >= compareAtMost)
                          }
                          onChange={event =>
                            setCompared(previous =>
                              event.target.checked
                                ? [...previous, run.id]
                                : previous.filter(item => item !== run.id),
                            )
                          }
                        />
                      )}
                    </td>
                    <td>
                      <button
                        className="lab-link strong"
                        onClick={() =>
                          setShown(
                            group ? { kind: "sweep", id: group } : { kind: "run", id: run.id },
                          )
                        }
                      >
                        {group
                          ? t("扫描 {id}（{count} 组）", { id: group, count: members.length })
                          : runName(run)}
                      </button>
                      <small className="lab-id">
                        {run.strategy}
                        {!group && run.label ? ` · ${run.id}` : ""}
                      </small>
                    </td>
                    <td>{run.symbols.join(listSeparator())}</td>
                    <td className="subtle">
                      {run.firstDay} – {run.lastDay} · {periodText(run.period)}
                    </td>
                    <td className="subtle lab-params" title={group ? "" : paramsText(run)}>
                      {group ? "" : paramsText(run)}
                    </td>
                    {columns.map(([key, , percent]) => (
                      <td
                        key={key}
                        className={`numeric ${key.endsWith("return") ? signed(run.metrics[key]) : ""}`}
                      >
                        {group ? "" : metric(run.metrics[key], percent)}
                      </td>
                    ))}
                    <td className="numeric">{group ? "" : (run.metrics.fills ?? "—")}</td>
                    <td>
                      {group ? (
                        t("已完成 {done} / {count}", {
                          done: members.filter(item => runState(item.state) === "succeeded").length,
                          count: members.length,
                        })
                      ) : (
                        <StatusBadge tone={runTones[runState(run.state)] ?? "neutral"}>
                          {t(states[runState(run.state)] ?? runState(run.state))}
                        </StatusBadge>
                      )}
                    </td>
                    <td className="subtle numeric">{started(run.startedMs)}</td>
                    <td>
                      {active ? (
                        <button className="lab-link" onClick={() => cancel(run)}>
                          {t("取消")}
                        </button>
                      ) : (
                        <DeleteRecord name={group || runName(run)} remove={() => remove(run.id)} />
                      )}
                    </td>
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

// A report's title: the run's name, which can be changed, and what it ran.
function RunHeader({
  run,
  id,
  back,
  renamed,
  cancel,
  remove,
}: {
  run?: RunSummary;
  id: string;
  back: () => void;
  renamed: () => Promise<void>;
  cancel?: () => void;
  remove: () => void;
}) {
  const [editing, setEditing] = useState<string | null>(null);
  const failure = useActionError();
  const save = () =>
    void failure.attempt(async () => {
      await lab({ case: "renameRun", value: { id, label: editing ?? "" } });
      setEditing(null);
      await renamed();
    });
  return (
    <header className="lab-report-header">
      <button className="lab-link" onClick={back}>
        {t("← 回测记录")}
      </button>
      {editing === null ? (
        <>
          <h3>{run ? runName(run) : id}</h3>
          <button className="lab-link" onClick={() => setEditing(run?.label ?? "")}>
            {t("重命名")}
          </button>
        </>
      ) : (
        <form
          className="lab-rename"
          onSubmit={event => {
            event.preventDefault();
            save();
          }}
        >
          <input
            aria-label={t("名称")}
            autoFocus
            maxLength={64}
            placeholder={id}
            value={editing}
            onChange={event => setEditing(event.target.value)}
          />
          <button type="submit" className="primary">
            {t("保存")}
          </button>
          <button type="button" onClick={() => setEditing(null)}>
            {t("取消")}
          </button>
        </form>
      )}
      {run && (
        <span className="subtle">
          {run.strategy} · {run.symbols.join(listSeparator())} · {run.firstDay} – {run.lastDay} ·{" "}
          {periodText(run.period)}
        </span>
      )}
      <span className="lab-spacer" />
      {run && runState(run.state) === "running" ? (
        <button className="lab-link" onClick={cancel}>
          {t("取消")}
        </button>
      ) : (
        <DeleteRecord name={run ? runName(run) : id} remove={remove} />
      )}
      <ActionError error={failure.error} />
    </header>
  );
}
