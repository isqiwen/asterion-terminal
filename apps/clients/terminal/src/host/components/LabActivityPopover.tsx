import { useEffect, useId, useRef, useState } from "react";
import { RunState, TaskKind, type LabTask } from "../../generated/asterion/lab_pb";
import { translate, type MessageValues } from "../../i18n";
import { Icon } from "../../ui/Icon";
import { StatusBadge, type Tone } from "../../ui/StatusBadge";
import { ActionError, useActionError } from "../action-error";
import { estimateRemainingMs, formatDuration, labTaskTitle, useLabActivity } from "../lab-activity";
import "./lab-activity.css";

const th = (key: string, values?: MessageValues) => translate("host", key, values);

const stateTone: Partial<Record<RunState, Tone>> = {
  [RunState.SUCCEEDED]: "ok",
  [RunState.FAILED]: "danger",
  [RunState.INTERRUPTED]: "attention",
  [RunState.STOPPED]: "attention",
};

const stateLabel: Partial<Record<RunState, string>> = {
  [RunState.RUNNING]: "运行中",
  [RunState.SUCCEEDED]: "已完成",
  [RunState.FAILED]: "失败",
  [RunState.INTERRUPTED]: "已中断",
  [RunState.STOPPED]: "已停止",
};

const shortTime = (ms: bigint) =>
  ms > 0n
    ? new Date(Number(ms)).toLocaleTimeString("sv-SE", {
        timeZone: "Asia/Shanghai",
        hour12: false,
      })
    : "";

type OpenHandlers = {
  openRun: (id: string) => void;
  openSweep: (id: string) => void;
  openFactors: (id?: string) => void;
};

function LabTasksPopover({
  panelId,
  anchor,
  placement,
  open,
  setOpen,
  trigger,
  openRun,
  openSweep,
  openFactors,
}: {
  panelId: string;
  anchor: string;
  placement: "up" | "down";
  open: boolean;
  setOpen: (value: boolean) => void;
  trigger: React.RefObject<HTMLButtonElement | null>;
} & OpenHandlers) {
  const activity = useLabActivity();
  const panel = useRef<HTMLElement>(null);
  const acted = useActionError();
  const [now, setNow] = useState(() => Date.now());
  const totalActive = activity?.totalActive ?? 0;

  useEffect(() => {
    if (!open || totalActive === 0) return;
    setNow(Date.now());
    const timer = window.setInterval(() => setNow(Date.now()), 1000);
    return () => window.clearInterval(timer);
  }, [open, totalActive]);

  if (!activity) return null;

  const jump = (task: LabTask) => {
    panel.current?.hidePopover?.();
    if (task.kind === TaskKind.BACKTEST) openRun(task.id);
    else if (task.kind === TaskKind.FACTOR)
      openFactors(task.state === RunState.SUCCEEDED ? task.id : undefined);
    else if (task.kind === TaskKind.FACTOR_BATCH) openFactors();
    else openSweep(task.id);
  };

  return (
    <section
      ref={panel}
      id={panelId}
      popover="auto"
      className={`lab-activity-panel is-${placement}`}
      aria-label={th("研究任务")}
      style={{ positionAnchor: anchor }}
      onToggle={event => setOpen(event.newState === "open")}
    >
      <header>
        <strong>{th("研究任务")}</strong>
        {activity.active.length > 0 && (
          <span className="subtle numeric">
            {th("运行 {running} / {max} 核", {
              running: activity.runningProcesses,
              max: Math.max(1, activity.maxProcesses),
            })}
            {activity.queuedRuns > 0 && ` · ${th("排队 {count}", { count: activity.queuedRuns })}`}
          </span>
        )}
        <span className="panel-spacer" />
        {activity.totalActive > 1 && (
          <button
            type="button"
            className="danger-quiet"
            onClick={() => void acted.attempt(() => activity.cancelAll())}
          >
            {th("全部取消")}
          </button>
        )}
        <button
          type="button"
          aria-label={th("关闭")}
          {...{ commandfor: panelId, command: "hide-popover" }}
          onClick={() => trigger.current?.focus()}
        >
          <Icon name="close" size={14} />
        </button>
      </header>
      <ActionError error={acted.error} namespace="asterion.terminal.research" />
      {open && (
        <div className="lab-activity-body">
          {activity.totalActive === 0 && activity.recent.length === 0 ? (
            <p className="subtle">{th("没有进行中的研究任务")}</p>
          ) : (
            <>
              {activity.totalActive > 0 && (
                <div className="lab-activity-group">
                  <h3>{th("进行中")}</h3>
                  <ul>
                    {activity.active.map(task => {
                      const started = Number(task.startedMs);
                      const elapsed = started > 0 ? formatDuration(now - started) : "";
                      const eta = estimateRemainingMs(task.done, task.total, started, now);
                      const multi = task.total > 1;
                      const pct = multi
                        ? Math.min(100, Math.floor((task.done * 100) / task.total))
                        : 0;
                      return (
                        <li key={task.id}>
                          <div className="lab-activity-row">
                            <div className="lab-activity-main">
                              <strong>{labTaskTitle(task)}</strong>
                              <span className="subtle numeric">
                                {[
                                  multi && `${task.done} / ${task.total} (${pct}%)`,
                                  task.failed > 0 && th("失败 {count}", { count: task.failed }),
                                  elapsed && th("已用 {time}", { time: elapsed }),
                                  eta !== null && th("剩余 ~{time}", { time: formatDuration(eta) }),
                                ]
                                  .filter(Boolean)
                                  .join(" · ")}
                              </span>
                              <progress
                                className="lab-activity-progress"
                                value={multi ? task.done : undefined}
                                max={multi ? task.total : undefined}
                              />
                            </div>
                            <div className="lab-activity-actions">
                              <button type="button" onClick={() => jump(task)}>
                                {th("查看")}
                              </button>
                              <button
                                type="button"
                                onClick={() => void acted.attempt(() => activity.cancelTask(task))}
                              >
                                {th("取消")}
                              </button>
                            </div>
                          </div>
                        </li>
                      );
                    })}
                  </ul>
                </div>
              )}
              {activity.recent.length > 0 && (
                <div className="lab-activity-group">
                  <h3>{th("最近完成")}</h3>
                  <ul>
                    {activity.recent.map(task => {
                      const duration =
                        task.finishedMs > task.startedMs && task.startedMs > 0n
                          ? formatDuration(Number(task.finishedMs - task.startedMs))
                          : "";
                      return (
                        <li key={task.id}>
                          <div className="lab-activity-row">
                            <div className="lab-activity-main">
                              <div className="lab-activity-title-line">
                                <StatusBadge tone={stateTone[task.state] ?? "neutral"}>
                                  {th(stateLabel[task.state] ?? "未知")}
                                </StatusBadge>
                                <strong>{labTaskTitle(task)}</strong>
                              </div>
                              <span className="subtle numeric" title={task.error || undefined}>
                                {[
                                  task.total > 1 && `${task.succeeded} / ${task.total}`,
                                  task.failed > 0 && th("失败 {count}", { count: task.failed }),
                                  duration,
                                  shortTime(task.finishedMs),
                                ]
                                  .filter(Boolean)
                                  .join(" · ")}
                              </span>
                              {task.error && (
                                <span className="lab-activity-error" title={task.error}>
                                  {task.error}
                                </span>
                              )}
                            </div>
                            <div className="lab-activity-actions">
                              <button type="button" onClick={() => jump(task)}>
                                {th("查看")}
                              </button>
                            </div>
                          </div>
                        </li>
                      );
                    })}
                  </ul>
                </div>
              )}
            </>
          )}
        </div>
      )}
    </section>
  );
}

// Compact progress pill in the bottom status bar; invisible when Lab is idle.
export function LabStatusTask(handlers: OpenHandlers) {
  const activity = useLabActivity();
  const panelId = useId();
  const anchor = `--lab-status-task-${panelId.replace(/:/g, "")}`;
  const trigger = useRef<HTMLButtonElement>(null);
  const [open, setOpen] = useState(false);

  if (!activity || activity.totalActive === 0) return null;

  const primary = activity.active[0]!;
  const extra = activity.totalActive - 1;
  const now = Date.now();
  const heading = labTaskTitle(primary);
  const done = primary.done;
  const total = primary.total;
  const startedMs = Number(primary.startedMs);
  const failed = primary.failed;

  const determinate = total > 1;
  const percent = determinate ? Math.min(100, Math.floor((done * 100) / total)) : 0;
  const eta = estimateRemainingMs(done, total, startedMs, now);
  const tooltip = [
    heading,
    determinate && `${done} / ${total}`,
    primary &&
      th("运行 {running} / {max} 核", {
        running: activity.runningProcesses,
        max: Math.max(1, activity.maxProcesses),
      }),
    activity.queuedRuns > 0 && th("排队 {count}", { count: activity.queuedRuns }),
    eta !== null && th("剩余 ~{time}", { time: formatDuration(eta) }),
    failed > 0 && th("失败 {count}", { count: failed }),
    extra > 0 && th("另有 {count} 个任务", { count: extra }),
  ]
    .filter(Boolean)
    .join(" · ");

  return (
    <div className="lab-activity-anchor">
      <button
        ref={trigger}
        type="button"
        className="status-task"
        title={tooltip}
        style={{ anchorName: anchor }}
        {...{ commandfor: panelId, command: "toggle-popover" }}
        aria-expanded={open}
      >
        <span className="status-task-label">
          {heading}
          {determinate ? ` ${done}/${total}` : ""}
          {extra > 0 ? ` +${extra}` : ""}
        </span>
        <progress
          className="status-progress"
          aria-label={heading}
          value={determinate ? done : undefined}
          max={determinate ? total : undefined}
        />
        {determinate && <span className="status-metric">{percent}%</span>}
      </button>
      <LabTasksPopover
        panelId={panelId}
        anchor={anchor}
        placement="up"
        open={open}
        setOpen={setOpen}
        trigger={trigger}
        {...handlers}
      />
    </div>
  );
}

// Quiet header entry on the Research module bar when active or session-recent tasks exist.
export function LabHeaderActivity(handlers: OpenHandlers) {
  const activity = useLabActivity();
  const panelId = useId();
  const anchor = `--lab-header-task-${panelId.replace(/:/g, "")}`;
  const trigger = useRef<HTMLButtonElement>(null);
  const [open, setOpen] = useState(false);

  if (!activity || (activity.totalActive === 0 && activity.recent.length === 0)) return null;

  return (
    <div className="lab-activity-anchor">
      <button
        ref={trigger}
        type="button"
        className="module-activity-trigger"
        data-active={activity.totalActive > 0 || undefined}
        style={{ anchorName: anchor }}
        {...{ commandfor: panelId, command: "toggle-popover" }}
        aria-expanded={open}
      >
        {th("研究任务")}
        {activity.totalActive > 0 && (
          <StatusBadge tone="attention">
            {th("{count} 运行中", { count: activity.totalActive })}
          </StatusBadge>
        )}
      </button>
      <LabTasksPopover
        panelId={panelId}
        anchor={anchor}
        placement="down"
        open={open}
        setOpen={setOpen}
        trigger={trigger}
        {...handlers}
      />
    </div>
  );
}
