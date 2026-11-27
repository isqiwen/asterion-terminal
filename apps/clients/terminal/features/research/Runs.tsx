import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type { TradingConnection } from "../../src/bridge/client";
import { lab } from "../../src/bridge/lab";
import { RunState, type RunSummary } from "../../src/generated/asterion/lab_pb";
import { ActionError, useActionError } from "../../src/host/action-error";
import type { TerminalContext } from "../../src/host/feature-registry";
import { notify } from "../../src/host/notifications";
import { useLabRunningPoll } from "../../src/host/lab";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { type MessageValues, translate, listSeparator } from "../../src/i18n";
import { StatusBadge } from "../../src/ui/StatusBadge";
import { DeleteRecord, useAnnotations } from "./LabParts";
import { ReconcileView } from "./ReconcileView";
import { SLIPPAGE_STRESS_TICKS } from "./research-analytics";
import {
  batchKey,
  identity,
  parameterText,
  recordName,
  recordTime,
  runName,
  runState,
  slippageKey,
  slippageStressBase,
} from "./research-lab";
import { saveAnnotation } from "./research-storage";
import { metric, periodText, runTones, shortTime, signed, states } from "./result-parts";
import { RunComparison } from "./RunComparison";
import { RunResult } from "./RunResult";
import { SweepResult } from "./SweepResult";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

// At most this many runs compare at once: more lines no longer read apart.
const compareAtMost = 6;
const pageSize = 50;
const columns = [
  ["total_return", "总收益", true],
  ["annual_return", "年化收益", true],
  ["sharpe", "夏普比率", false],
  ["max_drawdown", "最大回撤", true],
  ["calmar", "卡玛比率", false],
] as const;
type SortKey = "started" | "fills" | (typeof columns)[number][0];
type Shown =
  | { kind: "run" | "sweep"; id: string }
  | { kind: "compare"; ids: string[] }
  | { kind: "reconcile"; account?: string; instance?: string; run?: string; autoStart?: boolean }
  | null;

const paramsText = (run: RunSummary) =>
  Object.entries(run.params)
    .map(([name, value]) => `${name}=${parameterText(value)}`)
    .join(" ");

// Every run and sweep so far: a table to find, rank and compare them, and
// each one's report. The strategy view opens a new one: param `run` or `sweep`.
export function Runs(context: TerminalContext) {
  const [runs, setRuns] = useState<RunSummary[] | null>(null);
  const [strategies, setStrategies] = useState<string[]>([]);
  const [offset, setOffset] = useState(0);
  const [hasMore, setHasMore] = useState(false);
  const [loading, setLoading] = useState(false);
  const [openedRun, setOpenedRun] = useState<RunSummary>();
  const [shown, setShown] = useState<Shown>(null);
  const [sweep, setSweep] = useState<{
    id: string;
    runs: RunSummary[];
    selected: string;
    rolling?: RunSummary;
    active: boolean;
    pbo?: number;
    pboRankCorr?: number;
  } | null>(null);
  const [compared, setCompared] = useState<string[]>([]);
  const [strategy, setStrategy] = useWorkspaceDraft("runs-strategy", "");
  const [search, setSearch] = useWorkspaceDraft("runs-search", "");
  const [starredOnly, setStarredOnly] = useState(false);
  const annotations = useAnnotations();
  const [sort, setSort] = useState<{ key: SortKey; descending: boolean }>({
    key: "started",
    descending: true,
  });
  const acted = useActionError();
  const tradingAccounts = useMemo(
    () =>
      (context.snapshot?.trading_connections ?? []).filter(
        (item): item is TradingConnection => !("error" in item),
      ),
    [context.snapshot?.trading_connections],
  );
  const opened = context.workspaceParams;
  useEffect(() => {
    if (
      !opened?.run &&
      !opened?.sweep &&
      !opened?.compare &&
      !opened?.reconcileAccount &&
      !opened?.reconcileInstance &&
      !opened?.reconcileRun
    )
      return;
    if (opened.run) setShown({ kind: "run", id: opened.run });
    else if (opened.sweep) setShown({ kind: "sweep", id: opened.sweep });
    else if (opened.compare) {
      const ids = opened.compare
        .split(",")
        .map(item => item.trim())
        .filter(Boolean);
      if (ids.length >= 2) setShown({ kind: "compare", ids });
    } else if (opened.reconcileAccount || opened.reconcileInstance || opened.reconcileRun) {
      setShown({
        kind: "reconcile",
        account: opened.reconcileAccount,
        instance: opened.reconcileInstance,
        run: opened.reconcileRun,
        autoStart: Boolean(opened.reconcileAccount),
      });
    }
    context.navigate("workspace.lab-runs", {});
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [
    opened?.run,
    opened?.sweep,
    opened?.compare,
    opened?.reconcileAccount,
    opened?.reconcileInstance,
    opened?.reconcileRun,
  ]);

  const openedId = shown?.kind === "run" ? shown.id : undefined;
  const query = useMemo(
    () => ({
      page: {
        offset,
        limit: pageSize,
        strategy,
        search,
        orderBy: sort.key,
        ascending: !sort.descending,
      },
      openedId,
    }),
    [offset, strategy, search, sort, openedId],
  );
  const currentQuery = useRef<typeof query | null>(query);
  const readRuns = useCallback(async () => {
    setLoading(true);
    try {
      if (query.openedId) {
        const reply = await lab({ case: "getRunSummary", value: { id: query.openedId } });
        if (currentQuery.current === query) setOpenedRun(reply);
      } else {
        const reply = await lab({ case: "listRuns", value: query.page });
        if (currentQuery.current !== query) return;
        setRuns(reply.runs);
        setHasMore(reply.hasMore);
        setStrategies(reply.strategies);
      }
    } catch (failure) {
      if (currentQuery.current === query)
        notify(failure, { namespace: "asterion.terminal.research" });
    } finally {
      if (currentQuery.current === query) setLoading(false);
    }
  }, [query]);
  useEffect(() => {
    currentQuery.current = query;
    void readRuns();
    return () => {
      currentQuery.current = null;
    };
  }, [query, readRuns]);
  const running = openedId
    ? openedRun?.id === openedId && runState(openedRun.state) === "running"
    : (runs ?? []).some(run => runState(run.state) === "running" || run.sweepRunning) ||
      (shown?.kind === "sweep" && sweep?.id === shown.id && sweep.active);
  useLabRunningPoll(readRuns, running);
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
          rolling: reply.rolling,
          active: reply.active,
          pbo: reply.pbo,
          pboRankCorr: reply.pboRankCorr,
        });
      })
      .catch(failure => active && notify(failure, { namespace: "asterion.terminal.research" }));
    return () => {
      active = false;
    };
  }, [shown, runs]);

  const rows = starredOnly
    ? (runs ?? []).filter(run => Boolean(annotations[run.sweepId || run.id]?.starred))
    : (runs ?? []);
  const byId = new Map((runs ?? []).map(run => [run.id, run]));
  const back = () => {
    setShown(null);
    if (opened?.run || opened?.sweep) context.navigate("workspace.lab-runs", {});
  };
  const remove = (id: string) =>
    void acted.attempt(async () => {
      // A run of a sweep takes the whole sweep.
      await lab({ case: "deleteRun", value: { id } });
      setCompared(previous => previous.filter(item => item !== id));
      setShown(null);
      if (!openedId) await readRuns();
    });
  const cancel = (run: RunSummary) =>
    void acted.attempt(async () => {
      await (run.sweepId
        ? lab({ case: "cancelSweep", value: { id: run.sweepId } })
        : lab({ case: "cancelRun", value: { id: run.id } }));
      await readRuns();
    });
  const cancelSweep = (id: string) =>
    void acted.attempt(async () => {
      await lab({ case: "cancelSweep", value: { id } });
      const reply = await lab({ case: "getSweep", value: { id } });
      setSweep({
        id,
        runs: [...reply.runs],
        selected: reply.selectedId,
        rolling: reply.rolling,
        active: reply.active,
        pbo: reply.pbo,
        pboRankCorr: reply.pboRankCorr,
      });
      await readRuns();
    });

  const startSlippageStress = (runId: string) =>
    void acted.attempt(async () => {
      const detail = await lab({ case: "getRun", value: { id: runId } });
      const sweepId = identity(detail.summary?.strategy || "stress", "slip");
      const base = slippageStressBase(detail, context.snapshot?.research_directory ?? "");
      await lab({
        case: "startSweep",
        value: { id: sweepId, base, slippageTicks: [...SLIPPAGE_STRESS_TICKS], processes: 0 },
      });
      setShown({ kind: "sweep", id: sweepId });
    });

  if (shown?.kind === "run") {
    const run =
      openedRun?.id === shown.id
        ? openedRun
        : (byId.get(shown.id) ?? (sweep?.rolling?.id === shown.id ? sweep.rolling : undefined));
    const singleSucceeded = Boolean(
      run?.strategy &&
      run.state === RunState.SUCCEEDED &&
      (!run.sweepId || run.id !== `${run.sweepId}-rolling`),
    );
    return (
      <section className="lab-runs-view">
        <RunHeader
          run={run}
          id={shown.id}
          back={back}
          renamed={readRuns}
          loadIntoStrategy={() =>
            context.navigate("workspace.lab-strategies", { params: { loadRun: shown.id } })
          }
          slippageStress={singleSucceeded ? () => startSlippageStress(shown.id) : undefined}
          deploy={
            singleSucceeded
              ? () =>
                  context.navigate("workspace.trading-strategies", {
                    params: { run: run!.id },
                  })
              : undefined
          }
          cancel={run && (() => cancel(run))}
          remove={() => remove(shown.id)}
        />
        <ActionError error={acted.error} namespace="asterion.terminal.research" />
        <RunResult
          key={shown.id}
          id={shown.id}
          state={runState(run?.state ?? 0)}
          openVersion={version =>
            context.navigate("workspace.data-dataset", { params: { version } })
          }
          openDataset={params => context.navigate("workspace.data-dataset", { params })}
          openFactor={factorId =>
            context.navigate("workspace.lab-factors", { params: { factorResult: factorId } })
          }
          openRun={runId => setShown({ kind: "run", id: runId })}
        />
      </section>
    );
  }
  if (shown?.kind === "sweep") {
    const currentSweep = sweep?.id === shown.id ? sweep : null;
    const sweepSucceeded = currentSweep
      ? currentSweep.runs.filter(item => item.state === RunState.SUCCEEDED).length
      : 0;
    const sweepAnchor = currentSweep?.runs[0]?.id;
    const sweepTitle = currentSweep?.runs[0]?.params[batchKey]
      ? t("批量回测 {id}", { id: recordTime(shown.id) })
      : currentSweep?.runs[0]?.params[slippageKey]
        ? t("滑点压力测试 {id}", {
            id: recordName(shown.id, currentSweep?.runs[0]?.strategy.split(".")[1]),
          })
        : t("扫描 {id}", {
            id: recordName(shown.id, currentSweep?.runs[0]?.strategy.split(".")[1]),
          });
    return (
      <section className="lab-runs-view">
        <header className="lab-report-header">
          <button type="button" className="lab-link" onClick={back}>
            {t("← 回测记录")}
          </button>
          <h2>{sweepTitle}</h2>
          {currentSweep && (
            <StatusBadge
              tone={
                currentSweep.active
                  ? "neutral"
                  : sweepSucceeded === currentSweep.runs.length
                    ? "ok"
                    : "attention"
              }
            >
              {t("已完成 {done} / {count}", {
                done: sweepSucceeded,
                count: currentSweep.runs.length,
              })}
            </StatusBadge>
          )}
          <span className="lab-spacer" />
          {currentSweep?.active ? (
            <button type="button" className="lab-link" onClick={() => cancelSweep(shown.id)}>
              {t("取消")}
            </button>
          ) : (
            sweepAnchor && <DeleteRecord name={sweepTitle} remove={() => remove(sweepAnchor)} />
          )}
        </header>
        <ActionError error={acted.error} namespace="asterion.terminal.research" />
        {currentSweep ? (
          <SweepResult
            runs={currentSweep.runs}
            selected={currentSweep.selected}
            pbo={currentSweep.pbo}
            pboRankCorr={currentSweep.pboRankCorr}
            open={id => setShown({ kind: "run", id })}
            rolling={
              currentSweep.rolling || byId.has(`${shown.id}-rolling`)
                ? () =>
                    setShown({
                      kind: "run",
                      id: currentSweep.rolling?.id ?? `${shown.id}-rolling`,
                    })
                : undefined
            }
          />
        ) : (
          <p className="subtle">{t("正在读取结果…")}</p>
        )}
      </section>
    );
  }
  if (shown?.kind === "compare")
    return (
      <section className="lab-runs-view">
        <header className="lab-report-header">
          <button type="button" className="lab-link" onClick={back}>
            {t("← 回测记录")}
          </button>
          <h2>{t("比较 {count} 次运行", { count: shown.ids.length })}</h2>
        </header>
        <RunComparison
          ids={shown.ids}
          repository={context.snapshot?.research_directory ?? ""}
          onGenerated={(_path, id, scope) =>
            context.navigate("workspace.lab-strategies", {
              params: {
                openStrategy: id,
                symbols: scope.symbols,
                begin: scope.begin,
                end: scope.end,
              },
            })
          }
        />
      </section>
    );
  if (shown?.kind === "reconcile")
    return (
      <ReconcileView
        accounts={tradingAccounts}
        repository={context.snapshot?.research_directory ?? ""}
        initialAccount={shown.account}
        initialInstance={shown.instance}
        initialRun={shown.run}
        autoStart={shown.autoStart}
        back={back}
        openRun={id => setShown({ kind: "run", id })}
      />
    );

  const sortBy = (key: SortKey) => {
    setOffset(0);
    setSort(previous => ({
      key,
      descending:
        previous.key === key ? !previous.descending : !/drawdown|volatility|ulcer/.test(key),
    }));
  };
  const heading = (key: SortKey, label: string) => (
    <th
      scope="col"
      key={key}
      className="numeric"
      aria-sort={sort.key === key ? (sort.descending ? "descending" : "ascending") : undefined}
    >
      <button type="button" className="lab-sort" onClick={() => sortBy(key)}>
        {t(label)}
        <span aria-hidden="true">{sort.key === key ? (sort.descending ? " ↓" : " ↑") : ""}</span>
      </button>
    </th>
  );
  return (
    <section className="lab-runs-view">
      <search className="lab-toolbar">
        <select
          aria-label={t("策略")}
          value={strategy}
          onChange={event => {
            setOffset(0);
            setStrategy(event.target.value);
          }}
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
          spellCheck={false}
          autoCorrect="off"
          autoCapitalize="none"
          value={search}
          onChange={event => {
            setOffset(0);
            setSearch(event.target.value);
          }}
        />
        <button
          type="button"
          className={`lab-star-filter${starredOnly ? " is-active" : ""}`}
          aria-pressed={starredOnly}
          onClick={() => setStarredOnly(previous => !previous)}
        >
          ★ {t("仅看星标")}
        </button>
        <span className="lab-spacer" />
        {tradingAccounts.length > 0 && (
          <button type="button" onClick={() => setShown({ kind: "reconcile" })}>
            {t("实盘对账")}
          </button>
        )}
        {compared.length > 0 && (
          <button type="button" className="lab-link" onClick={() => setCompared([])}>
            {t("清空选择")}
          </button>
        )}
        <button
          type="button"
          className="primary"
          disabled={compared.length < 2}
          onClick={() => setShown({ kind: "compare", ids: compared })}
        >
          {t("比较所选（{count}）", { count: compared.length })}
        </button>
      </search>
      <ActionError error={acted.error} namespace="asterion.terminal.research" />
      {!runs ? (
        <p className="subtle" role="status">
          {t("读取中")}
        </p>
      ) : !rows.length ? (
        <p className="lab-placeholder">{t(strategies.length ? "没有匹配的运行" : "还没有运行")}</p>
      ) : (
        <div
          className="lab-scroll"
          tabIndex={0}
          role="region"
          aria-label={t("回测记录")}
          aria-busy={loading}
        >
          <table className="lab-table lab-records" aria-label={t("回测记录")}>
            <thead>
              <tr>
                <th scope="col">
                  <span className="sr-only">{t("比较")}</span>
                </th>
                <th scope="col">{t("名称")}</th>
                <th scope="col">{t("品种")}</th>
                <th scope="col">{t("区间")}</th>
                <th scope="col">{t("参数")}</th>
                {columns.map(([key, label]) => heading(key, label))}
                {heading("fills", "成交笔数")}
                <th scope="col">{t("状态")}</th>
                {heading("started", "开始时间")}
                <th scope="col">
                  <span className="sr-only">{t("操作")}</span>
                </th>
              </tr>
            </thead>
            <tbody>
              {rows.map(run => {
                const group = run.sweepId;
                const active = group ? run.sweepRunning : runState(run.state) === "running";
                const comparable = !group && runState(run.state) === "succeeded";
                const checked = !group && compared.includes(run.id);
                const openRow = () =>
                  setShown(group ? { kind: "sweep", id: group } : { kind: "run", id: run.id });
                return (
                  <tr
                    key={group || run.id}
                    className={checked ? "selected" : undefined}
                    onClick={openRow}
                  >
                    <td onClick={event => event.stopPropagation()}>
                      {!group && (
                        <input
                          type="checkbox"
                          aria-label={t("比较 {id}", { id: runName(run) })}
                          checked={checked}
                          disabled={!comparable || (!checked && compared.length >= compareAtMost)}
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
                      <div className="lab-run-title">
                        <button
                          type="button"
                          className="lab-link strong"
                          onClick={event => {
                            event.stopPropagation();
                            openRow();
                          }}
                        >
                          {group
                            ? run.sweepStrategies
                              ? t("批量 {id}（{count} 个策略）", {
                                  id: recordTime(group),
                                  count: run.sweepStrategies,
                                })
                              : t("扫描 {id}（{count} 组）", {
                                  id: recordName(group, run.strategy.split(".")[1]),
                                  count: run.sweepSize,
                                })
                            : runName(run)}
                        </button>
                        {!group && (
                          <button
                            type="button"
                            className={`lab-star${annotations[run.id]?.starred ? " is-starred" : ""}`}
                            aria-pressed={Boolean(annotations[run.id]?.starred)}
                            aria-label={t(annotations[run.id]?.starred ? "取消星标" : "加星标")}
                            onClick={event => {
                              event.stopPropagation();
                              saveAnnotation(run.id, { starred: !annotations[run.id]?.starred });
                            }}
                          >
                            {annotations[run.id]?.starred ? "★" : "☆"}
                          </button>
                        )}
                      </div>
                      <small className="lab-id">
                        {run.sweepStrategies ? "" : run.strategy}
                        {!group && run.label ? ` · ${run.id}` : ""}
                        {!group && annotations[run.id]?.note
                          ? ` · ${annotations[run.id]!.note}`
                          : ""}
                      </small>
                    </td>
                    <td>{run.symbols.join(listSeparator())}</td>
                    <td className="subtle">
                      {run.firstDay} – {run.lastDay} · {periodText(run.period)}
                    </td>
                    <td className="subtle lab-params" title={group ? "" : paramsText(run)}>
                      {group ? "—" : paramsText(run)}
                    </td>
                    {columns.map(([key, , percent]) => (
                      <td
                        key={key}
                        className={`numeric ${group ? "subtle" : key.endsWith("return") ? signed(run.metrics[key]) : ""}`}
                      >
                        {group ? "—" : metric(run.metrics[key], percent)}
                      </td>
                    ))}
                    <td className={`numeric ${group ? "subtle" : ""}`}>
                      {group ? "—" : (run.metrics.fills ?? "—")}
                    </td>
                    <td>
                      {group ? (
                        <StatusBadge
                          tone={
                            active
                              ? "neutral"
                              : run.sweepSucceeded === run.sweepSize
                                ? "ok"
                                : "attention"
                          }
                        >
                          {t("已完成 {done} / {count}", {
                            done: run.sweepSucceeded,
                            count: run.sweepSize,
                          })}
                        </StatusBadge>
                      ) : (
                        <StatusBadge tone={runTones[runState(run.state)] ?? "neutral"}>
                          {states[runState(run.state)]
                            ? t(states[runState(run.state)])
                            : runState(run.state)}
                        </StatusBadge>
                      )}
                    </td>
                    <td className="subtle numeric">{shortTime(run.startedMs)}</td>
                    <td onClick={event => event.stopPropagation()}>
                      {active ? (
                        <button type="button" className="lab-link" onClick={() => cancel(run)}>
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
      {(offset > 0 || hasMore) && (
        <nav className="lab-toolbar" aria-label={t("回测记录分页")}>
          <button
            type="button"
            disabled={loading || offset === 0}
            onClick={() => setOffset(offset - pageSize)}
          >
            {t("上一页")}
          </button>
          {rows.length > 0 && (
            <span className="subtle numeric">
              {t("第 {first}–{last} 条", { first: offset + 1, last: offset + rows.length })}
            </span>
          )}
          <button
            type="button"
            disabled={loading || !hasMore}
            onClick={() => setOffset(offset + pageSize)}
          >
            {t("下一页")}
          </button>
        </nav>
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
  loadIntoStrategy,
  slippageStress,
  deploy,
  cancel,
  remove,
}: {
  run?: RunSummary;
  id: string;
  back: () => void;
  renamed: () => Promise<void>;
  loadIntoStrategy: () => void;
  slippageStress?: () => void;
  deploy?: () => void;
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
      <button type="button" className="lab-link" onClick={back}>
        {t("← 回测记录")}
      </button>
      {editing === null ? (
        <>
          <h2>{run ? runName(run) : recordName(id)}</h2>
          <button type="button" className="lab-link" onClick={() => setEditing(run?.label ?? "")}>
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
            placeholder={run ? runName({ ...run, label: "" }) : recordName(id)}
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
      <button type="button" className="lab-link" onClick={loadIntoStrategy}>
        {t("带入策略配置")}
      </button>
      {slippageStress && (
        <button type="button" className="lab-link" onClick={slippageStress}>
          {t("滑点压力测试")}
        </button>
      )}
      {deploy && (
        <button type="button" className="lab-link" onClick={deploy}>
          {t("去部署")}
        </button>
      )}
      {run && runState(run.state) === "running" ? (
        <button type="button" className="lab-link" onClick={cancel}>
          {t("取消")}
        </button>
      ) : (
        <DeleteRecord name={run ? runName(run) : recordName(id)} remove={remove} />
      )}
      <ActionError error={failure.error} namespace="asterion.terminal.research" />
    </header>
  );
}
